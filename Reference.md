# How-to: Bus & Env

Writing an extension (`ext/*`) or a script-facing tool that needs to discover devices or store/retrieve state.  
This is the *practical* contract, for the mechanical/spec detail see `core/bus.h`, `core/dev/env.h`, `core/mask.h`, and the README's in-depth sections.  

The concept:  
1. Extensions and dynamic objects are **guest** on the kernel, do not get raw pointers, physical addresses,  
   or port I/O, talks to the kernel through the bus (typed ops) and env (handles + strings + masks).  
3. The bus table is **read-only after boot**. Register during `ext_init_all()` and never again. There is no lock; there is no unregister.

---

## 1. The Bus, "who is here, and how do I call it"

### What it is (and is not)
A name → ops registry, linear with the core. `bus_register()` copies a small descriptor into a static table.  
Each device class has its **own** ops struct (`console_ops`, `timer_ops`, `env_ops`, `vfs_ops`, …);  
the bus stores `void *ops` plus a `class` tag and casts on lookup.  
There is **no** uniform `read/write/ioctl`, a timer is not a block device, and pretending otherwise buys nothing.

### Two consumers, two styles
- **Static C extensions / core modules**:
  keep calling your module's direct API (`timer_after()`, `env->set()`). Routing through the bus buys you nothing.
  The bus exists for discovery, not for dispatch.

- **The script VM / proxy**:
  cannot `#include` kernel headers, so it discovers devices at runtime via `bus_find()` / `bus_find_class()`,
  and calls through the proxy's handle+opcode boundary.  

### The lifecycle
```

boot
 └─ serial_init()          (very first, before the bus)
 └─ bus_init()             (zero the table; runs before heap_init)
 └─ gdt/idt/pic, pmm/vmm/heap
 └─ timer_init()  ──┐
 └─ env_init()    ──┤  wave 1: core modules, kmain order
 └─ console/vfs/… ──┘      each calls bus_register() from its own init
 └─ ext_init_all()        wave 2: extensions, priority-ordered
       └─ each ext calls bus_register()  (and may publish via env)
 └─ bus_settle()          publish manifest into env; table becomes read-only
 └─ main super-loop       bus_find()/typed finders are lock-free reads
```

Registration is **best-effort**:  
A failed `bus_register()` (table full, dup name, `NULL` ops) means your device is reachable only through its direct API.  
Check the return if discovery matters to you; otherwise ignore it.  

### Registering

```c
#include <core/bus.h>

static const struct my_ops my_bus_ops = {
    .do_thing = my_do_thing,
    .get_stat = my_get_stat,
};

/* from your ext init, after your internals are ready */
static void my_ext_init(void) {
    /* ... your own setup ... */

    struct bus_device dev = {
        .name         = "myext",          /* unique, string literal, lives forever */
        .class        = BUS_CLASS_STAT,   /* must be non-NONE, must match ops */
        .version      = 1,
        .ops          = &my_bus_ops,      /* must NOT be NULL; must match class */
        .private_data = my_state,         /* opaque to the bus */
        .owner_id     = BUS_MYEXT_ID,     /* from config.h, fits 8 bits */
    };
    if (bus_register(&dev) != 0)
        klog_warning("myext: bus registration failed\n");

    bus_test("myext");   /* boot-time sanity probe: confirms the bus saw you */
}
```

**Rules of registration**
- `name`: non-empty, unique. Keep the string alive forever (string literals fine).
- `ops`: must point to a struct matching `class`. The bus does **not** verify this,
  a wrong-class cast is silent UB. `private_data` likewise must outlive the entry.
- `owner_id`: from the authoritative table in `config.h`.
  This becomes the `OWNER` field when the bus publishes your manifest entry into env.
  It is **not** the same as `class` (bus classification), and **not** the same as env-mask `TYPE` (value kind).
  The numbers overlapping 0–9 is coincidence.
- Only happens once and never after the shell session has begun.

### Bus: Discover & Look-up Mechanics

```c
/* by name */
const struct bus_device *d = bus_find("console");

/* typed finder, first of a class */
const struct timer_ops *t = bus_timer();
const struct env_ops   *e = bus_env();
const struct console_ops *c = bus_console();
const struct vfs_ops   *v = bus_vfs();

/* kth of a class (0 = first) */
const struct bus_device *second = bus_find_class(BUS_CLASS_CONSOLE, 1);

/* iteration for `lsbus`-style tools */
for (size_t i = 0; i < bus_count(); i++) {
    const struct bus_device *dev = bus_at(i);
    /* dev->name, dev->class, dev->version, dev->owner_id */
}
```

Every lookup is a lock-free read and is safe from **any** context, including ISRs, *after* `bus_settle()`.  
Before settle, only registration is running.

Typed finders return `NULL` if no device of that class is registered.  
Always null-check, `bus_owner_id(NULL)` returns `BUS_KERN_ID` (a safe default).  

### Adding a new device class
Append to `enum bus_class` in `core/bus.h`, add a `struct <class>_ops`, add a typed finder `bus_<class>()`,  
and register with that class, do not reuse an existing class for a device that doesn't match its ops layout.  

---

## 2. Env, "where state lives, and how to keep it honest"

### Environment Mechanics
A bounded, fixed-capacity store of `(namespace, name) → value | [mask]`.
Reached as `bus_env()`. Read-mostly by intent: system info, session state, flags, paths, small payloads.  
It is **not** a filesystem, **not** an access-control system, and **not** a queue.  

### The one rule that prevents most bugs
> **Env cannot see who is calling.** It has no notion of "current process."  
> Anything that needs an ownership gate must implement it in the caller (the proxy does this).
> `unset()` and `drop_namespace()` are deliberately not gated by `READONLY`/`PINNED` for this reason.  

### Environment: write

```c
const struct env_ops *env = bus_env();
if (env == NULL) return;   /* env not registered, degrade gracefully */

int rc = env->set(
    "myapp.session",       /* namespace */
    "mode",                /* name */
    "fast",                /* value (NUL-terminated string) */
    0,                     /* access_budget: 0 = unlimited */
    0,                     /* ttl_ticks: 0 = no expiry */
    ENV_IMPORTANCE_DEFAULT,
    env_mask_build(BUS_MYEXT_ID, ENV_TYPE_STRING, 0, 0)
);

if (rc == ENV_NO_SPACE) { /* store full or heap exhausted */ }
if (rc == ENV_READONLY) { /* entry (or its PINNED owner) refused the write */ }
```

**`set()` is all-or-nothing.**  
If it returns non-`ENV_OK`, the store is exactly as it was: no entry was evicted, the previous value is intact.  
You never have to "check if it half-worked."

### Environment: read

```c
char buf[64];
size_t len = 0;
int rc = env->get("myapp.session", "mode", buf, sizeof buf, &len);
/* ENV_OK                → buf holds the value, *len = length */
/* ENV_NOT_FOUND         → absent or expired (expired reads as absent) */
/* ENV_BUFFER_TOO_SMALL  → *len holds the needed length; access budget NOT consumed */
/* ENV_INVALID_ARGS      → NULL/empty args or buf_max == 0 */
```

- **`get()` consumes one access** from the entry's budget (if it has one).
  **`peek()` does not.** `stat()` does not, and never touches `last_access`.
- **Reads are exact whether or not the sweeper has run.**
  An expired entry always reads as absent; the sweeper only reclaims the memory and the slot.
- **`ENV_BUFFER_TOO_SMALL` is not a failure**, retry with the reported length.

### Environment: enumerating

```c
/* one namespace, names only */
size_t it = 0;
char name[ENV_NAME_MAX + 1];
while (env->list("myapp.session", &it, name, sizeof name) == ENV_OK) {
    /* ... */
}

/* every namespace, non-SECRET entries only */
it = 0;
char ns[ENV_NAME_MAX + 1], nm[ENV_NAME_MAX + 1];
while (env->list_all(&it, ns, sizeof ns, nm, sizeof nm) == ENV_OK) { /* ... */ }
```

Iteration walks **slot order**, so inserts/removals between calls may cause
skips or repeats. Treat `*iter` as opaque, restart at `0` for a fresh walk, and
`ENV_NOT_FOUND` means "end."

---

## 3.  a 32-bit Identity field and mask metadata

```
 31      24 23    20 19    16 15                 0
+----------+--------+--------+-------------------+
|  OWNER   |  TYPE  | FLAGS  |     PRIVATE       |
| (8 bits) |(4 bits)|(4 bits)|    (16 bits)      |
+----------+--------+--------+-------------------+
```

Build it with `env_mask_build(owner, type, flags, priv)`, **never** shift by hand.  
The flags argument takes the **unshifted** `ENV_FLAG_*` constants (`ENV_FLAG_READONLY` etc.);  
`env_mask_flags()` returns them **shifted down**  
(0..15). Test with `env_mask_has(mask, ENV_FLAG_*)`, never by comparing `env_mask_flags()` against `ENV_FLAG_*`.  

| Field | Who fills it | What env does |
|---|---|---|
| `OWNER` | you, with your `BUS_*_ID` | policy checks (PINNED overwrite) |
| `TYPE` | you, `ENV_TYPE_*` | **nothing**, a hint for tools/UI |
| `FLAGS` | you, `ENV_FLAG_*` | `READONLY`, `SECRET`, `PINNED`, `CACHEABLE` |
| `PRIVATE` | you (or a co-processor) | **nothing**, opaque to env |

**PRIVATE is claimed by whoever owns the value's format.**  
The decimal module (`ext/script/decimal.c`) takes the whole bottom 16 bits as `SCALE | ROUND | FLAGS`.  
If you store a decimal via `dec_set`, do not also write PRIVATE bits yourself, you'll corrupt its metadata.  
If you store your own type, use `ENV_Q_*`-friendly bit patterns and document them.  

### Flag semantics (short version)

| Flag | Meaning | Enforced |
|---|---|---|
| `READONLY` | set once, never overwritten by anyone | yes |
| `PINNED` | never a space-pressure victim; only **owner** may overwrite | yes |
| `SECRET` | hidden from `list`, `list_all`, and any query whose `care` omits the bit | yes |
| `CACHEABLE` | safe to recompute; evicted before same-importance non-cacheable | yes |

> **`SECRET` is not access control.** Reads by exact name are **not** hidden.
> If you expose env to untrusted code (the proxy), gate it there.

---

## 4. Querying by mask

Two query forms exist, and **they are not interchangeable**:  

### Care/want, exact, use this for `OWNER` and `TYPE`

```c
size_t n;
env->count_match(ENV_Q_OWNER(BUS_VM16), &n);        /* exactly vm16's entries */
env->count_match(ENV_Q_TYPE(ENV_TYPE_INT), &n);     /* exactly int-typed */
env->count_match(ENV_Q_FLAG(ENV_FLAG_PINNED), &n);  /* PINNED entries */

/* list form: same first two args, plus iterator and buffers */
size_t it = 0;
char ns[64], nm[64];
while (env->list_match(ENV_Q_OWNER(BUS_VM16), /* want */ BUS_VM16 << 24,
                       &it, ns, sizeof ns, nm, sizeof nm) == ENV_OK) { /* ... */ }
```

`ENV_Q_*` expands to **two** arguments `(care, want)`, matching `(mask & care) == want`.  
**Do not** pass them to `list_by_mask`, that's a different contract (see below).  

> Combining terms: OR the **care** halves and OR the **want** halves yourself.  
> OR-ing two `ENV_Q_FLAG` terms requires **both** flags (AND), not "either."  

### Subset match, legacy, flags and private bits only

```c
env->list_by_mask(ENV_FLAG_PINNED, &it, ...);   /* correct: flag bits */
env->count_by_mask(ENV_FLAG_SECRET, &n);        /* correct: flag bits */
```

This matches entries whose mask **contains all bits** of `query_mask`.  
It is only meaningful for `FLAGS` and `PRIVATE`. **Never** use it for `OWNER` or `TYPE`,  
owner `1` is a bit-subset of owners `3`, `5`, `7`, so `list_by_mask(ENV_Q_OWNER(1))` would return the wrong entries.  

### The O(1) short-circuit

Env keeps an exact index (`bit_cnt[32]`, `active_mask_or`, `owner_cnt[256]`, `type_cnt[16]`) updated on every set and reclaim.  
A query that cannot possibly match is rejected in O(1) without touching an entry.  
**The index is a filter, never an answer**: a zero counter means "provably nothing matches";  
a non-zero means "a scan is required." You don't call it directly, `count_match` / `list_match` use it internally.  

---

## 5. Eviction, priorities & Lifecycles

When the store is full, victims are chosen in this order:  

1. **expired** entries (even `PINNED`, the TTL already won)
2. **lowest importance**, but only entries with `importance <= incoming`
   (a low-importance write can never displace a more valuable entry)
3. **`CACHEABLE`** before non-cacheable, at equal importance
4. **least recently accessed**, then name order

> Live `PINNED` entries are **never** victims.  
> A write that cannot succeed without evicting an ineligible entry fails with `ENV_NO_SPACE` and **nothing** is evicted.  

### Environment: importance levels

| Value | Use for |
|---|---|
| `0`–`3` | ephemeral cache, recomputable derived state (mark `CACHEABLE`) |
| `4`–`6` | `ENV_IMPORTANCE_DEFAULT`, normal session/shell state |
| `7`–`9` | important config, paths, identities |
| `10` (`ENV_IMPORTANCE_MAX`) | effectively persistent against eviction |

`importance` is **not** `PINNED`. `PINNED` also enforces "only owner may overwrite," which is stronger.  
If you truly need "never evicted, only I may replace it," set `PINNED`.  
If you need "very sticky but freely overwritable," use `ENV_IMPORTANCE_MAX`.  

### Environment: Budgets

- **Access budget** (`access_budget > 0`): entry is reclaimed after N `get()`s.
  `peek`/`stat` never consume. Use for one-shot tokens, hand-off values.
- **TTL** (`ttl_ticks > 0`): entry expires `ttl_ticks` after `set()`.
  Saturating, it can never wrap into "already expired." Use for leases, caches, heartbeats.  

---

## 6. Publishing a manifest (the pattern `bus.c` uses)

The bus publishes every device as `kernel.dev.<name>.{id,class,version}` under owner `BUS_BUS_ID`,  
flags `READONLY | PINNED`. Copy that shape when your extension has discoverable state:  

```c
static void my_ext_publish(void) {
    const struct env_ops *env = bus_env();
    if (env == NULL) return;

    const char *ns = "kernel.ext.myext";
    uint32_t m = env_mask_build(BUS_MYEXT_ID, ENV_TYPE_INT,
                                ENV_FLAG_READONLY | ENV_FLAG_PINNED, 0);

    char buf[16];
    ksnprintf(buf, sizeof buf, "%u", MY_VERSION);
    (void)env->set(ns, "version", buf, 0, 0, ENV_IMPORTANCE_DEFAULT, m);
}
```

Publish **once**, at the end of your init, after your internals are ready.  
Re-publishing an entry you've already marked `READONLY | PINNED` as a *non-owner* will fail with `ENV_READONLY`,  
which is correct behavior, not a bug to work around.  

---

## 7. The proxy (experimental)

Scripted objects or `vm16.c/LC-3` programs cannot `#include <core/bus.h>` and cannot hold raw pointers (ideally).  
The proxy (`ext/proxy/`) attempts to provide a better safe-call boundary:  

```c
const struct proxy_ops *p = bus_proxy();
uint32_t h;
if (p->bind("console", &h) == PROXY_OK) {
    const char msg[] = "Hello, World!\0";
    p->call(h, PROXY_CONSOLE_WRITE, (const uint8_t *)msg, sizeof msg,
            NULL, 0, NULL);
    p->unbind(h);
}
```

- `bind(name, &handle)`, leases an opaque handle to a registered device.  
- `call(handle, opcode, in, in_len, out, out_len, out_used)`, dispatch.  
- `unbind(handle)`, release.  

**The proxy enforces ownership**  
that env cannot: for `env->set`/`unset` through the proxy, the caller's mask `OWNER` must match the entry's.  
That's the gate `env` itself cannot provide. If you write a new proxy opcode that touches env, implement the same gate.  

---

## 8. Debugging the framework

| Symptom | Likely cause | Look at |
|---|---|---|
| `bus_find("x")` returns NULL | registration failed (dup name, table full, `NULL` ops) or you registered after settle | your init's `bus_register` return; `bus_test("x")` in boot log |
| typed finder returns NULL but device is up | wrong `class` on the `bus_device` | your `bus_register` call vs. `enum bus_class` |
| `env->set` returns `ENV_READONLY` unexpectedly | entry is `READONLY`, or `PINNED` and you're not the owner | `env->stat(...)` → `mask`; `env_mask_owner(mask)` vs your `BUS_*_ID` |
| `env->set` returns `ENV_NO_SPACE` with free slots | no *eligible* victim (all `PINNED`, or higher importance than your write) | `env->info(...)`; check importance of what's there |
| `list_by_mask` returns wrong OWNER/TYPE matches | you used subset match for a number field | switch to `list_match` + `ENV_Q_OWNER`/`ENV_Q_TYPE` |
| `env->get` succeeds on an entry that should be gone | TTL not set, or you're reading by exact name (`SECRET` only hides from enumeration) | `env->stat(...)` → `has_ttl`, `ttl_remaining` |
| `dec_get` returns `ok == false` | not `ENV_TYPE_INT`, wrong owner, bad scale/round/flags in PRIVATE, or payload isn't canonical int64 | `env->stat` → `mask`; decode `DEC_P_SCALE/ROUND/FLAGS` |
| your entry vanishes under pressure | importance too low, or `CACHEABLE` set | raise importance; consider `PINNED` |

---

## 10. Summary

**Bus**: register once at init (`bus_register`, name + class + ops + owner_id), never after settle; look up by name (`bus_find`),  
or class (`bus_find_class`/typed finders); after settle it's a lock-free read-only table safe from any context.  
**Env**: `bus_env()` gives you `set`/`get`/`peek`/`unset`/`stat`/`list`/`list_all`/`list_match`/`count_match` (and legacy `*_by_mask` for flags only);  
masks are built with `env_mask_build` and tested with `env_mask_has`; `set` is all-or-nothing and never evicts ineligible entries;  
`OWNER` and `TYPE` are **numbers**, query them with care/want, not subset; `SECRET` hides from enumeration,  
not from exact reads; `PINNED`/`READONLY` are enforced, `unset`/`drop_namespace` are not gated (caller's job);  
`PRIVATE` is owned by whoever owns the value's format (decimal takes the bottom 16 bits).  
If scripts need any of this, they go through the **proxy**, which enforces the ownership gate env cannot.  

---
