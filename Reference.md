# Bus & Environment Reference

A practical guide for core modules and extensions that need to discover devices or store small pieces of state. This document describes the intended API contract; the headers are authoritative for declarations and configuration values:

- `kernel/src/core/bus.h`
- `kernel/src/core/dev/env.h`
- `kernel/src/core/mask.h`
- `kernel/src/core/config.h`

> **Scope and safety:** The bus and environment are kernel facilities, not a process-isolation mechanism. The environment has no caller identity and does not enforce general access control. Native extensions execute in the kernel's address space; code that handles untrusted or script-provided data must validate it and enforce its own policy.

## At a glance

- **Bus:** a boot-time registry of named devices and class-specific ops tables. It is for discovery, not a universal dispatch interface.
- **Environment (`env`):** a bounded key/value store keyed by `(namespace, name)`, with metadata, expiry, access budgets, and space-pressure eviction.
- **Scripts:** use the proxy's handle/opcode interface rather than kernel headers or native pointers. Treat the proxy as a boundary that must validate each operation; the environment itself cannot identify a caller.

## 1. Bus

### What the bus provides

The bus stores a small copy of each `struct bus_device` in a static table. Each entry contains a name, class, version, ops pointer, opaque private-data pointer, and owner ID. The bus does not define a common `read`/`write`/`ioctl` interface: each class has its own ops structure, and the caller must use the structure that matches the device's class.

Statically linked core modules and C extensions should normally call their module API directly. Use the bus when a consumer needs to discover a device dynamically, such as a script-facing proxy.

### Initialization and registration

The intended boot order is:

1. Initialize the bus before modules register devices.
2. Initialize core devices; each registers during its own initialization.
3. Initialize extensions; they may register devices and publish environment state.
4. Call `bus_settle()` after registration is complete. It attempts to publish the device manifest and marks the intended end of registration.
5. Consumers perform lookups.

Registration is intended for boot initialization only. There is no unregister operation or locking, and callers must not register concurrently with lookups. The implementation does not reject `bus_register()` after `bus_settle()`; late registration is nevertheless outside the contract and can invalidate assumptions made by readers.

`bus_register()` returns `0` on success and `-1` on failure (for example: invalid descriptor, duplicate name, or full table). It copies the descriptor, not the objects its pointers refer to: `name`, `ops`, and `private_data` must remain valid for the lifetime of the registration. Registration is best-effort, so check the return value if discovery is required.

```c
#include <core/bus.h>

static const struct my_ops my_ops = {
    .do_thing = my_do_thing,
};

static void my_ext_init(void) {
    /* Initialize my_state and all internals before publishing the device. */
    struct bus_device dev = {
        .name         = "myext",       /* unique; keep the string alive */
        .class        = BUS_CLASS_STAT, /* must match the ops structure */
        .version      = 1,
        .ops          = &my_ops,
        .private_data = my_state,       /* must remain valid */
        .owner_id     = BUS_MYEXT_ID,   /* owner ID from config.h */
    };

    if (bus_register(&dev) != 0)
        klog_warning("myext: bus registration failed\n");
}
```

The bus checks that the name is non-empty, the class is not `BUS_CLASS_NONE`, and `ops` is non-NULL. It does **not** check that the ops pointer has the correct layout for the class. A mismatch can result in undefined behavior when a typed finder casts the pointer.

Keep these identifiers distinct:

- `owner_id` identifies the device owner and is used as the environment mask's `OWNER` when the bus publishes its manifest.
- `class` identifies the bus device class.
- Environment `TYPE` describes a value for tools; it is not the bus class or owner ID.

### Looking up devices

```c
const struct bus_device *d = bus_find("console");
const struct bus_device *first_console =
    bus_find_class(BUS_CLASS_CONSOLE, 0);
const struct bus_device *second_console =
    bus_find_class(BUS_CLASS_CONSOLE, 1);

const struct timer_ops *timer = bus_timer();
const struct env_ops *env = bus_env();

for (size_t i = 0; i < bus_count(); i++) {
    const struct bus_device *dev = bus_at(i);
    /* Inspect dev->name, dev->class, dev->version, dev->owner_id. */
}
```

Lookups return `NULL` when no matching device exists; always check before dereferencing. `bus_owner_id(NULL)` returns `BUS_KERN_ID`, but that convenience default is not a substitute for checking whether lookup succeeded.

After registration has finished, bus lookups are lock-free reads and are documented as safe from any context, including ISRs. **This does not apply to environment operations**, which have separate context restrictions.

To add a device class, define its class value, ops structure, and typed finder, then register devices with that class. Do not reuse a class for an incompatible ops layout. See `core/bus.h` for the current class list and API.

## 2. Environment store

### Model and limits

The environment stores string values under `(namespace, name)` keys. It has fixed slot capacity and a configured maximum payload size; limits such as `ENV_NAME_MAX` and `ENV_VALUE_MAX` are defined in configuration. It is useful for small system, session, and extension state. It is not a filesystem, queue, or access-control system.

Obtain the ops table through `bus_env()` and handle the possibility that it is unavailable:

```c
const struct env_ops *env = bus_env();
if (env == NULL) {
    /* The environment device is unavailable; degrade or report an error. */
    return;
}
```

Environment operations allocate memory and are not synchronized. The header specifies **thread context only; never call them from an ISR**. This includes operations that may appear to be reads or enumerations.

### Writing and replacing values

```c
int rc = env->set(
    "myapp.session",       /* namespace */
    "mode",                /* name */
    "fast",                /* NUL-terminated value string */
    0,                     /* access_budget: 0 means unlimited */
    0,                     /* ttl_ticks: 0 means no expiry */
    ENV_IMPORTANCE_DEFAULT,
    env_mask_build(BUS_MYEXT_ID, ENV_TYPE_STRING, 0, 0)
);

if (rc == ENV_NO_SPACE) {
    /* No eligible space, or heap allocation failed. */
} else if (rc == ENV_READONLY) {
    /* Existing entry is READONLY, or PINNED by a different owner. */
} else if (rc != ENV_OK) {
    /* Handle other errors, including invalid arguments or missing device. */
}
```

`set()` stages the new strings before applying space-pressure eviction. If it cannot make enough room using eligible victims, it returns `ENV_NO_SPACE` without evicting those victims. One edge case matters: an **expired entry with the same key is reclaimed before the new value is staged**, so a later failure can still leave that already-expired entry removed. Expired entries are unreadable regardless of whether the background sweeper has reclaimed them.

`READONLY` prevents replacement through `set()` by everyone, including the original owner. `PINNED` prevents eviction while live and allows replacement only when the new mask's owner matches the existing entry's owner. These rules do not make `unset()` or `drop_namespace()` safe from arbitrary callers; see the security note below.

### Reading values

```c
char buf[64];
size_t len = 0;
int rc = env->get("myapp.session", "mode", buf, sizeof buf, &len);

if (rc == ENV_OK) {
    /* buf is NUL-terminated; len is the value length excluding NUL. */
} else if (rc == ENV_BUFFER_TOO_SMALL) {
    /* len is the required value length; allocate len + 1 and retry. */
} else if (rc == ENV_NOT_FOUND) {
    /* Key is absent or expired. */
}
```

The destination size includes space for the terminating NUL. `len_out` is optional. `get()` consumes one access from a positive access budget only after a successful copy; when the final access is consumed, the entry is reclaimed. `peek()` does not consume the budget. A too-small buffer returns `ENV_BUFFER_TOO_SMALL` and does not consume an access.

A successful `get()` or `peek()` updates `last_access`, which is used by eviction's least-recently-accessed tie-breaker. `stat()` does not consume a budget or update `last_access`. Expired values read as absent even if the sweeper has not run; the sweeper reclaims storage, not validity.

### Enumerating entries

```c
size_t it = 0;
char name[ENV_NAME_MAX + 1];
while (env->list("myapp.session", &it, name, sizeof name) == ENV_OK) {
    /* Use name. */
}

it = 0;
char ns[ENV_NAME_MAX + 1];
char nm[ENV_NAME_MAX + 1];
while (env->list_all(&it, ns, sizeof ns, nm, sizeof nm) == ENV_OK) {
    /* Use ns and nm. */
}
```

`list()` returns names in one namespace; `list_all()` returns namespace/name pairs. Both hide `SECRET` entries by default. Names are truncated if the output buffer is too small, so use buffers sized for the configured maximums when the full key is needed.

Enumeration walks slot order, not a snapshot. Inserts or removals between calls may cause skips or repeats. Initialize the iterator to zero for a fresh walk; treat it as opaque. `ENV_NOT_FOUND` means the walk is finished.

### Caller identity and security

> **The environment cannot see who is calling.** It has no current-process identity and does not enforce ownership for reads, `unset()`, or `drop_namespace()`.

`SECRET` only hides an entry from enumeration when the query does not request the secret bit; it does not hide exact-name reads. `READONLY` and `PINNED` constrain `set()` and eviction, but do not prevent `unset()` or namespace removal. Any interface exposed to untrusted callers must implement the required ownership and access checks itself. In particular, do not expose core-only `drop_namespace()` through an untrusted proxy without a deliberate authorization check.

## 3. Mask metadata

Build masks with `env_mask_build()`; do not shift fields by hand. The 32-bit layout is:

```
 31      24 23    20 19    16 15                 0
+----------+--------+--------+-------------------+
|  OWNER   |  TYPE  | FLAGS  |     PRIVATE       |
| (8 bits) |(4 bits)|(4 bits)|    (16 bits)      |
+----------+--------+--------+-------------------+
```

| Field | Set by | Meaning to the environment |
|---|---|---|
| `OWNER` | Caller, normally a `BUS_*_ID` | Used for the `PINNED` replacement-owner check. |
| `TYPE` | Caller, using `ENV_TYPE_*` | Descriptive metadata for tools; not validated against the value. |
| `FLAGS` | Caller, using `ENV_FLAG_*` | Controls the policies below. |
| `PRIVATE` | Owner of the value format | Opaque to the environment; available to format-specific code. |

The `flags` argument to `env_mask_build()` uses the constants such as `ENV_FLAG_READONLY` in their defined bit positions. `env_mask_flags()` returns a compact, shifted-down value; do not compare that result directly with `ENV_FLAG_*`. Use `env_mask_has(mask, ENV_FLAG_...)` to test flags.

| Flag | Behavior |
|---|---|
| `READONLY` | Existing entry cannot be replaced with `set()`. It can still be removed with `unset()` or `drop_namespace()`. |
| `PINNED` | A live entry is not an eviction victim; only a write with the same owner may replace it. |
| `SECRET` | Hidden from default enumeration, but not from exact-name reads. |
| `CACHEABLE` | Preferred over non-cacheable entries as an eviction victim at equal importance. |

`PRIVATE` belongs to the format that uses it. For example, the decimal module uses the full low 16 bits for its own scale, rounding, and flags metadata. Do not independently assign those bits to values managed by that format.

## 4. Querying by mask

The exact `care`/`want` form and legacy subset matching are different contracts.

### Exact matching: use for `OWNER` and `TYPE`

An exact query matches when `(mask & care) == want`. The `ENV_Q_*` helpers expand to the two arguments expected by `list_match()` and `count_match()`:

```c
size_t n;
env->count_match(ENV_Q_OWNER(BUS_VM16), &n);       /* exact owner */
env->count_match(ENV_Q_TYPE(ENV_TYPE_INT), &n);    /* exact type */
env->count_match(ENV_Q_FLAG(ENV_FLAG_PINNED), &n); /* has PINNED */

size_t it = 0;
char ns[ENV_NAME_MAX + 1], name[ENV_NAME_MAX + 1];
while (env->list_match(ENV_Q_OWNER(BUS_VM16), &it,
                       ns, sizeof ns, name, sizeof name) == ENV_OK) {
    /* Entries owned by BUS_VM16. */
}
```

Do not pass `ENV_Q_*` to `list_by_mask()` or `count_by_mask()`. To combine exact terms, OR the `care` parts together and OR the corresponding `want` parts together. Combining two flag terms this way requires both flags, not either one. For `SECRET`, a query whose `care` includes the secret bit may enumerate secret entries; setting `want` to zero for that bit selects non-secret entries instead.

### Subset matching: flags/private bits

`list_by_mask()` and `count_by_mask()` find entries containing **all** bits in `query_mask`. This is useful for flag and private-bit subsets, not numeric `OWNER` or `TYPE` values.

```c
env->count_by_mask(ENV_FLAG_SECRET, &n);
```

Using subset matching for an owner or type is incorrect: for example, owner value `1` shares bits with several other owner values. Use `ENV_Q_OWNER()` or `ENV_Q_TYPE()` with the exact-match functions.

The implementation maintains indexes that can quickly reject some impossible exact queries. These indexes are an internal optimization: a possible match still requires a scan, so callers should use the query APIs rather than treating the index as a result.

## 5. Budgets, expiry, and eviction

### Access budget and TTL

- `access_budget == 0`: unlimited successful `get()` calls. A positive budget reclaims the entry after that many successful gets. `peek()` and `stat()` do not consume the budget.
- `ttl_ticks == 0`: no expiry. A positive TTL makes the entry unreadable at its expiry time; a sweeper reclaims its slot and memory later. Expiry arithmetic saturates rather than wrapping.

### Importance and eviction

Importance is clamped to `ENV_IMPORTANCE_MAX` and higher values are more valuable. When space is needed, the store prefers victims in this order:

1. Expired entries, including expired `PINNED` entries.
2. Lowest importance among live entries whose importance is no greater than the incoming write's importance.
3. `CACHEABLE` before non-cacheable at equal importance.
4. Least recently accessed; ties are broken by name order.

Live `PINNED` entries are never victims. If enough eligible space cannot be made, the write fails with `ENV_NO_SPACE` rather than evicting a more valuable or pinned live entry. Importance is not equivalent to `PINNED`: maximum importance makes an entry difficult to evict, not impossible to evict or owner-restricted.

## 6. Bus manifest and extension metadata

At `bus_settle()`, the bus attempts to publish each registered device under namespace `kernel.dev.<device-name>`, with `id`, `class`, and `version` keys. These entries use owner `BUS_BUS_ID` and `READONLY | PINNED` flags. Manifest publication is best-effort: environment unavailability, capacity limits, or other `set()` failures can leave entries unpublished. The manifest is diagnostic/discovery metadata, not a replacement for `bus_find()`.

An extension can publish its own metadata after initialization. For immutable metadata, use a unique namespace and publish once:

```c
static void my_ext_publish(void) {
    const struct env_ops *env = bus_env();
    if (env == NULL)
        return;

    uint32_t mask = env_mask_build(
        BUS_MYEXT_ID, ENV_TYPE_INT,
        ENV_FLAG_READONLY | ENV_FLAG_PINNED, 0);

    char value[16];
    ksnprintf(value, sizeof value, "%u", MY_VERSION);
    int rc = env->set("kernel.ext.myext", "version", value,
                      0, 0, ENV_IMPORTANCE_DEFAULT, mask);
    if (rc != ENV_OK)
        klog_warning("myext: metadata publication failed (%d)\n", rc);
}
```

Because `READONLY` prevents all replacements, including by the same owner, immutable entries cannot be republished or updated with `set()`. Omit `READONLY` if the value is meant to change; retain appropriate ownership and authorization checks for any exposed write path.

## 7. Proxy notes

Script-facing code should use the proxy's opaque handles and opcodes rather than kernel pointers or direct environment ops. The proxy is experimental; consult its current ops definition and implementation for supported devices, opcodes, payload formats, and return codes. Every operation that reads or mutates environment state must apply its intended authorization policy in the proxy, because the environment cannot determine the caller.

## 8. Troubleshooting

| Symptom | Check |
|---|---|
| `bus_find()` returns `NULL` | Check `bus_register()`'s return value, duplicate names, table capacity, and that registration happened during initialization. |
| Typed finder returns `NULL` | Check the registered class and whether any device of that class was registered. |
| `env->set()` returns `ENV_READONLY` | Inspect the old entry's flags and owner with `stat()`; `READONLY` blocks all replacement, while `PINNED` blocks replacement from a different owner. |
| `env->set()` returns `ENV_NO_SPACE` despite free slots | Check byte capacity and whether any eviction victim is eligible at the incoming importance; heap allocation can also fail. |
| `list_by_mask()` gives unexpected owner/type results | Use exact `list_match()`/`count_match()` queries with `ENV_Q_OWNER()` or `ENV_Q_TYPE()`. |
| A `SECRET` value is readable by key | Expected: `SECRET` only affects enumeration, not exact reads. |
| A value expires but still occupies capacity | Expired values are unreadable immediately but may remain allocated until a sweep or another reclaim operation. |
| A value is evicted | Check importance, `CACHEABLE`, access recency, and whether it is live and `PINNED`. |

## Summary

Register bus devices during initialization, keep their referenced objects alive, and use class-correct ops. Use `bus_env()` for small string values; build masks with `env_mask_build()`, query numeric owner/type fields with care/want helpers, and remember that exact reads are not hidden by `SECRET`. The environment has no caller identity: enforce authorization at the interface that exposes it, and do not call its operations from an ISR.
