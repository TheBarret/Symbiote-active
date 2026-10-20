#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <lib/mem.h>
#include <lib/string.h>
#include <core/arch/cpu.h>
#include <core/arch/heap.h>
#include <core/dev/timer.h>
#include <core/dev/env.h>
#include <core/panic.h>
#include <core/klog.h>
#include <core/bus.h>

/* entry */

struct env_entry {
    char        *ns;            /* owned; NULL when slot is free */
    char        *name;          /* owned */
    char        *value;         /* owned */

    uint32_t    ns_len;
    uint32_t    name_len;
    uint32_t    value_len;

    uint32_t    access_remaining;   /* 0 = unlimited */
    uint64_t    expires_at;         /* ticks; 0 = no time budget */
    uint64_t    last_access;        /* ticks at last read or write */
    uint8_t     importance;         /* higher = more valuable */
    uint32_t    mask;               /* opaque, caller-defined classification bits */

    bool        live;
};

enum env_reclaim_reason {
    ENV_RECLAIM_ACCESS,
    ENV_RECLAIM_TIME,
    ENV_RECLAIM_SPACE,
    ENV_RECLAIM_EXPLICIT,
};

/* device state */

struct env_device {
    struct env_entry *entries;
    size_t             capacity;         /* number of slots */
    size_t             count;            /* live entries */
    size_t             used_bytes;       /* sum of ns_len + name_len + value_len */
    size_t             max_bytes;

    const struct timer_ops *timer;       /* NULL if no timer device */
    int                sweep_id;         /* timer task id, or -1 */
    size_t             sweep_pos;        /* sweeper resume cursor (slot index) */

    uint64_t n_set;
    uint64_t n_get;
    uint64_t n_peek;
    uint64_t n_unset;
    uint64_t n_reclaim_access;
    uint64_t n_reclaim_time;
    uint64_t n_reclaim_space;
    uint64_t n_reclaim_explicit;
    uint64_t n_sweeps;
};

static struct env_device *g_env;

/* forward decls */

static void env_sweep(void *arg);

/* helpers */

static inline size_t env_footprint(const struct env_entry *s) {
    return (size_t)s->ns_len + s->name_len + s->value_len;
}

static uint64_t env_now(struct env_device *e) {
    return (e->timer != NULL) ? e->timer->ticks() : 0;
}

static bool env_expired(struct env_device *e, const struct env_entry *slot) {
    if (!slot->live) return true;
    if (slot->expires_at == 0) return false;
    return env_now(e) >= slot->expires_at;
}

static struct env_entry *env_find(struct env_device *e,
                                  const char *ns, size_t ns_len,
                                  const char *name, size_t name_len) {
    for (size_t i = 0; i < e->capacity; i++) {
        struct env_entry *s = &e->entries[i];
        if (!s->live) continue;
        if (env_expired(e, s)) continue;
        if (s->ns_len != ns_len) continue;
        if (s->name_len != name_len) continue;
        if (memcmp(s->ns, ns, ns_len) != 0) continue;
        if (memcmp(s->name, name, name_len) != 0) continue;
        return s;
    }
    return NULL;
}

static struct env_entry *env_find_free_slot(struct env_device *e) {
    for (size_t i = 0; i < e->capacity; i++) {
        if (!e->entries[i].live) return &e->entries[i];
    }
    return NULL;
}

static void env_reclaim(struct env_device *e, struct env_entry *slot,
                        enum env_reclaim_reason reason) {
    if (!slot->live) return;

    size_t freed = env_footprint(slot);
    if (freed <= e->used_bytes) e->used_bytes -= freed;
    else e->used_bytes = 0;

    kfree(slot->ns);
    kfree(slot->name);
    kfree(slot->value);

    slot->ns = NULL;
    slot->name = NULL;
    slot->value = NULL;
    slot->ns_len = slot->name_len = slot->value_len = 0;
    slot->access_remaining = 0;
    slot->expires_at = 0;
    slot->last_access = 0;
    slot->importance = 0;
    slot->mask = 0;
    slot->live = false;

    if (e->count > 0) e->count--;

    switch (reason) {
    case ENV_RECLAIM_ACCESS:   e->n_reclaim_access++;   break;
    case ENV_RECLAIM_TIME:     e->n_reclaim_time++;     break;
    case ENV_RECLAIM_SPACE:    e->n_reclaim_space++;    break;
    case ENV_RECLAIM_EXPLICIT: e->n_reclaim_explicit++; break;
    }
}

static void env_reap_expired(struct env_device *e) {
    for (size_t i = 0; i < e->capacity; i++) {
        struct env_entry *s = &e->entries[i];
        if (s->live && env_expired(e, s))
            env_reclaim(e, s, ENV_RECLAIM_TIME);
    }
}

/* Eviction order: lowest importance first (higher = more valuable),
 * then least recently accessed, then name as a deterministic tiebreak.
 * `exclude` (may be NULL) is never chosen. */
static struct env_entry *env_pick_victim(struct env_device *e,
                                         const struct env_entry *exclude) {
    struct env_entry *victim = NULL;
    for (size_t i = 0; i < e->capacity; i++) {
        struct env_entry *s = &e->entries[i];
        if (!s->live || s == exclude) continue;
        if (victim == NULL) { victim = s; continue; }
        if (s->importance < victim->importance) { victim = s; continue; }
        if (s->importance > victim->importance) continue;
        if (s->last_access < victim->last_access) { victim = s; continue; }
        if (s->last_access > victim->last_access) continue;
        if (strcmp(s->name, victim->name) < 0) victim = s;
    }
    return victim;
}

/* Make room for `footprint` bytes. If `existing` is non-NULL it is being
 * replaced in place: its bytes are credited and its slot is reused, and it is
 * never evicted. Caller guarantees footprint <= max_bytes, so this terminates
 * with success once every other entry has been evicted. */
static bool env_make_room(struct env_device *e, size_t footprint,
                          const struct env_entry *existing) {
    size_t credit = (existing != NULL) ? env_footprint(existing) : 0;
    if (credit > e->used_bytes) credit = e->used_bytes;

    for (;;) {
        bool need_slot  = (existing == NULL) && (env_find_free_slot(e) == NULL);
        bool need_bytes = (e->used_bytes - credit + footprint > e->max_bytes);
        if (!need_slot && !need_bytes) return true;

        struct env_entry *victim = env_pick_victim(e, existing);
        if (victim == NULL) return false;
        env_reclaim(e, victim, ENV_RECLAIM_SPACE);
    }
}

/* ops */

static int env_set(const char *ns, const char *name, const char *value,
                   uint32_t access_budget, uint64_t ttl_ticks,
                   uint8_t importance, uint32_t mask) {
    struct env_device *e = g_env;
    if (e == NULL) return ENV_NO_DEVICE;
    if (ns == NULL || name == NULL || value == NULL) return ENV_INVALID_ARGS;

    size_t ns_len    = strlen(ns);
    size_t name_len  = strlen(name);
    size_t value_len = strlen(value);
    if (ns_len == 0 || name_len == 0) return ENV_INVALID_ARGS;
    if (name_len > ENV_NAME_MAX || ns_len > ENV_NAME_MAX) return ENV_INVALID_ARGS;
    if (value_len > ENV_VALUE_MAX) return ENV_INVALID_ARGS;

    if (importance > ENV_IMPORTANCE_MAX) importance = ENV_IMPORTANCE_MAX;

    size_t footprint = ns_len + name_len + value_len;
    if (footprint > e->max_bytes) return ENV_NO_SPACE;   /* can never fit: evict nothing */

    /* Stage the new strings first so a failure leaves the store untouched. */
    char *n_ns    = kmalloc(ns_len + 1);
    char *n_name  = kmalloc(name_len + 1);
    char *n_value = kmalloc(value_len + 1);
    if (n_ns == NULL || n_name == NULL || n_value == NULL) {
        kfree(n_ns); kfree(n_name); kfree(n_value);
        return ENV_NO_SPACE;
    }
    memcpy(n_ns, ns, ns_len);          n_ns[ns_len] = '\0';
    memcpy(n_name, name, name_len);    n_name[name_len] = '\0';
    memcpy(n_value, value, value_len); n_value[value_len] = '\0';

    /* Reap first so a stale duplicate can't coexist with the new entry. */
    env_reap_expired(e);
    struct env_entry *existing = env_find(e, ns, ns_len, name, name_len);

    if (!env_make_room(e, footprint, existing)) {
        kfree(n_ns); kfree(n_name); kfree(n_value);
        return ENV_NO_SPACE;
    }

    struct env_entry *slot = existing;
    if (slot == NULL) {
        slot = env_find_free_slot(e);
        if (slot == NULL) {
            kfree(n_ns); kfree(n_name); kfree(n_value);
            return ENV_INTERNAL;
        }
        e->count++;
    } else {
        size_t old = env_footprint(slot);
        e->used_bytes = (old <= e->used_bytes) ? e->used_bytes - old : 0;
        kfree(slot->ns); kfree(slot->name); kfree(slot->value);
    }

    slot->ns = n_ns;
    slot->name = n_name;
    slot->value = n_value;
    slot->ns_len = (uint32_t)ns_len;
    slot->name_len = (uint32_t)name_len;
    slot->value_len = (uint32_t)value_len;

    uint64_t now = env_now(e);
    slot->access_remaining = access_budget;
    slot->expires_at = (ttl_ticks == 0) ? 0 : now + ttl_ticks;
    slot->last_access = now;
    slot->importance = importance;
    slot->mask = mask;
    slot->live = true;

    e->used_bytes += footprint;
    e->n_set++;
    return ENV_OK;
}

static int env_get_common(const char *ns, const char *name,
                          char *buf, size_t buf_max, size_t *len_out,
                          bool consume) {
    struct env_device *e = g_env;
    if (e == NULL) return ENV_NO_DEVICE;
    if (ns == NULL || name == NULL || buf == NULL || buf_max == 0)
        return ENV_INVALID_ARGS;

    size_t ns_len = strlen(ns), name_len = strlen(name);
    if (ns_len == 0 || name_len == 0) return ENV_INVALID_ARGS;

    struct env_entry *s = env_find(e, ns, ns_len, name, name_len);
    if (s == NULL) return ENV_NOT_FOUND;

    if (len_out != NULL) *len_out = s->value_len;
    if (buf_max < (size_t)s->value_len + 1) return ENV_BUFFER_TOO_SMALL; /* nothing consumed */

    memcpy(buf, s->value, s->value_len);
    buf[s->value_len] = '\0';
    s->last_access = env_now(e);

    if (!consume) {
        e->n_peek++;
        return ENV_OK;
    }

    e->n_get++;
    if (s->access_remaining == 1)
        env_reclaim(e, s, ENV_RECLAIM_ACCESS);   /* caller already holds a copy */
    else if (s->access_remaining > 1)
        s->access_remaining--;
    return ENV_OK;
}

static int env_get(const char *ns, const char *name,
                   char *buf, size_t buf_max, size_t *len_out) {
    return env_get_common(ns, name, buf, buf_max, len_out, true);
}

static int env_peek(const char *ns, const char *name,
                    char *buf, size_t buf_max, size_t *len_out) {
    return env_get_common(ns, name, buf, buf_max, len_out, false);
}

static int env_unset(const char *ns, const char *name) {
    struct env_device *e = g_env;
    if (e == NULL) return ENV_NO_DEVICE;
    if (ns == NULL || name == NULL) return ENV_INVALID_ARGS;

    struct env_entry *s = env_find(e, ns, strlen(ns), name, strlen(name));
    if (s == NULL) return ENV_NOT_FOUND;

    env_reclaim(e, s, ENV_RECLAIM_EXPLICIT);
    e->n_unset++;
    return ENV_OK;
}

static int env_is_exist(const char *ns, const char *name) {
    struct env_device *e = g_env;
    if (e == NULL) return ENV_NO_DEVICE;
    if (ns == NULL || name == NULL) return ENV_INVALID_ARGS;

    struct env_entry *s = env_find(e, ns, strlen(ns), name, strlen(name));
    return (s != NULL) ? ENV_OK : ENV_NOT_FOUND;
}

static int env_stat(const char *ns, const char *name, struct env_stat *out) {
    struct env_device *e = g_env;
    if (e == NULL) return ENV_NO_DEVICE;
    if (ns == NULL || name == NULL || out == NULL) return ENV_INVALID_ARGS;

    struct env_entry *s = env_find(e, ns, strlen(ns), name, strlen(name));
    if (s == NULL) return ENV_NOT_FOUND;

    uint64_t now = env_now(e);
    out->value_len        = s->value_len;
    out->access_remaining = s->access_remaining;
    out->has_ttl          = (s->expires_at != 0);
    out->ttl_remaining    = (s->expires_at > now) ? s->expires_at - now : 0;
    out->last_access      = s->last_access;
    out->importance       = s->importance;
    out->mask             = s->mask;
    return ENV_OK;
}

static int env_info(struct env_info *out) {
    struct env_device *e = g_env;
    if (e == NULL) return ENV_NO_DEVICE;
    if (out == NULL) return ENV_INVALID_ARGS;

    out->capacity           = e->capacity;
    out->count              = e->count;
    out->used_bytes         = e->used_bytes;
    out->max_bytes          = e->max_bytes;
    out->timer_attached     = (e->timer != NULL);
    out->n_set              = e->n_set;
    out->n_get              = e->n_get;
    out->n_peek             = e->n_peek;
    out->n_unset            = e->n_unset;
    out->n_reclaim_access   = e->n_reclaim_access;
    out->n_reclaim_time     = e->n_reclaim_time;
    out->n_reclaim_space    = e->n_reclaim_space;
    out->n_reclaim_explicit = e->n_reclaim_explicit;
    out->n_sweeps           = e->n_sweeps;
    return ENV_OK;
}

/* Index-resume listing: O(N) total across a full enumeration. */
static int env_list(const char *ns, size_t *iter,
                    char *name_out, size_t name_max) {
    struct env_device *e = g_env;
    if (e == NULL) return ENV_NO_DEVICE;
    if (ns == NULL || iter == NULL || name_out == NULL) return ENV_INVALID_ARGS;
    if (name_max == 0) return ENV_INVALID_ARGS;

    size_t ns_len = strlen(ns);
    for (size_t i = *iter; i < e->capacity; i++) {
        struct env_entry *s = &e->entries[i];
        if (!s->live) continue;
        if (env_expired(e, s)) continue;
        if (s->ns_len != ns_len) continue;
        if (memcmp(s->ns, ns, ns_len) != 0) continue;

        size_t copy = s->name_len;
        if (copy >= name_max) copy = name_max - 1;
        memcpy(name_out, s->name, copy);
        name_out[copy] = '\0';

        *iter = i + 1;
        return ENV_OK;
    }
    *iter = e->capacity;
    return ENV_NOT_FOUND;
}

static void env_emit_names(const struct env_entry *s,
                           char *ns_out, size_t ns_max,
                           char *name_out, size_t name_max) {
    size_t ns_copy = s->ns_len;
    if (ns_copy >= ns_max) ns_copy = ns_max - 1;
    memcpy(ns_out, s->ns, ns_copy);
    ns_out[ns_copy] = '\0';

    size_t name_copy = s->name_len;
    if (name_copy >= name_max) name_copy = name_max - 1;
    memcpy(name_out, s->name, name_copy);
    name_out[name_copy] = '\0';
}

static int env_list_all(size_t *iter,
                        char *ns_out, size_t ns_max,
                        char *name_out, size_t name_max) {
    struct env_device *e = g_env;
    if (e == NULL) return ENV_NO_DEVICE;
    if (iter == NULL || ns_out == NULL || name_out == NULL) return ENV_INVALID_ARGS;
    if (ns_max == 0 || name_max == 0) return ENV_INVALID_ARGS;

    for (size_t i = *iter; i < e->capacity; i++) {
        struct env_entry *s = &e->entries[i];
        if (!s->live) continue;
        if (env_expired(e, s)) continue;

        env_emit_names(s, ns_out, ns_max, name_out, name_max);
        *iter = i + 1;
        return ENV_OK;
    }
    *iter = e->capacity;
    return ENV_NOT_FOUND;
}

/* Cross-namespace bitmask query. The mask is opaque to env: an entry matches
 * when it carries every bit of query_mask. A zero query is rejected rather
 * than silently matching the whole store. */
static int env_list_by_mask(uint32_t query_mask, size_t *iter,
                            char *ns_out, size_t ns_max,
                            char *name_out, size_t name_max) {
    struct env_device *e = g_env;
    if (e == NULL) return ENV_NO_DEVICE;
    if (query_mask == 0) return ENV_INVALID_ARGS;
    if (iter == NULL || ns_out == NULL || name_out == NULL) return ENV_INVALID_ARGS;
    if (ns_max == 0 || name_max == 0) return ENV_INVALID_ARGS;

    for (size_t i = *iter; i < e->capacity; i++) {
        struct env_entry *s = &e->entries[i];
        if (!s->live) continue;
        if (env_expired(e, s)) continue;
        if ((s->mask & query_mask) != query_mask) continue;

        env_emit_names(s, ns_out, ns_max, name_out, name_max);
        *iter = i + 1;
        return ENV_OK;
    }
    *iter = e->capacity;
    return ENV_NOT_FOUND;
}

static int env_count_by_mask(uint32_t query_mask, size_t *count_out) {
    struct env_device *e = g_env;
    if (e == NULL) return ENV_NO_DEVICE;
    if (query_mask == 0 || count_out == NULL) return ENV_INVALID_ARGS;

    size_t n = 0;
    for (size_t i = 0; i < e->capacity; i++) {
        struct env_entry *s = &e->entries[i];
        if (!s->live || env_expired(e, s)) continue;
        if ((s->mask & query_mask) == query_mask) n++;
    }
    *count_out = n;
    return ENV_OK;
}

static int env_drop_namespace(const char *ns) {
    struct env_device *e = g_env;
    if (e == NULL) return ENV_NO_DEVICE;
    if (ns == NULL) return ENV_INVALID_ARGS;

    size_t ns_len = strlen(ns);
    for (size_t i = 0; i < e->capacity; i++) {
        struct env_entry *s = &e->entries[i];
        if (!s->live) continue;
        if (s->ns_len != ns_len) continue;
        if (memcmp(s->ns, ns, ns_len) != 0) continue;
        env_reclaim(e, s, ENV_RECLAIM_EXPLICIT);
    }
    return ENV_OK;
}

/* sweeper: examines at most ENV_SWEEP_BATCH slots per pass, resuming where the
 * previous pass stopped, so every slot is visited within capacity/BATCH passes. */

static void env_sweep(void *arg) {
    struct env_device *e = arg;
    if (e == NULL || e->timer == NULL || e->capacity == 0) return;

    uint64_t now = e->timer->ticks();
    size_t n = (e->capacity < ENV_SWEEP_BATCH) ? e->capacity : ENV_SWEEP_BATCH;

    for (size_t k = 0; k < n; k++) {
        struct env_entry *s = &e->entries[(e->sweep_pos + k) % e->capacity];
        if (s->live && s->expires_at != 0 && now >= s->expires_at)
            env_reclaim(e, s, ENV_RECLAIM_TIME);
    }
    e->sweep_pos = (e->sweep_pos + n) % e->capacity;

    e->n_sweeps++;
    e->sweep_id = e->timer->after(ENV_SWEEP_PERIOD_MS, env_sweep, e);
    if (e->sweep_id < 0)
        klog_warning("Environment sweeper could not reschedule; lazy expiry only\n");
}

void env_destroy(void) {
    struct env_device *e = g_env;
    if (e == NULL) return;

    if (e->timer != NULL && e->sweep_id >= 0)
        e->timer->cancel(e->sweep_id);
    e->sweep_id = -1;

    for (size_t i = 0; i < e->capacity; i++) {
        if (e->entries[i].live) env_reclaim(e, &e->entries[i], ENV_RECLAIM_EXPLICIT);
    }

    g_env = NULL;       /* ops now return ENV_NO_DEVICE */
    kfree(e->entries);
    kfree(e);
}

/* bus registration */

static const struct env_ops env_bus_ops = {
    .set            = env_set,
    .get            = env_get,
    .peek           = env_peek,
    .unset          = env_unset,
    .is_exist       = env_is_exist,
    .stat           = env_stat,
    .info           = env_info,
    .list           = env_list,
    .list_all       = env_list_all,
    .list_by_mask   = env_list_by_mask,
    .count_by_mask  = env_count_by_mask,
    .drop_namespace = env_drop_namespace,
};

// Ref: defined at bus.h
//const struct env_ops *bus_env(void) {
//    const struct bus_device *d = bus_find_class(BUS_CLASS_ENV, 0);
//    return (d != NULL) ? (const struct env_ops *)d->ops : NULL;
//}

/* init */

void env_init(size_t max_entries, size_t max_bytes) {
    if (max_entries == 0) max_entries = ENV_DEFAULT_MAX_ENTRIES;
    if (max_bytes   == 0) max_bytes   = ENV_DEFAULT_MAX_BYTES;
    if (max_entries > SIZE_MAX / sizeof(struct env_entry))
        PANIC("env: slot table size overflow");

    struct env_device *e = kzalloc(sizeof *e);
    if (e == NULL) PANIC("env: cannot allocate device");

    e->entries = kzalloc(max_entries * sizeof *e->entries);
    if (e->entries == NULL) PANIC("env: cannot allocate slot table");

    e->capacity   = max_entries;
    e->max_bytes  = max_bytes;
    e->sweep_id   = -1;
    e->sweep_pos  = 0;

    g_env = e;

    e->timer = bus_timer();
    if (e->timer != NULL) {
        e->sweep_id = e->timer->after(ENV_SWEEP_PERIOD_MS, env_sweep, e);
        if (e->sweep_id < 0)
            klog_warning("Environment sweep timer not enabled\n");
    }

    klog("→ env_init() entries=%zu, bytes=%zu, timer=%s\n",
            max_entries, max_bytes,
            e->timer ? "attached" : "disabled");

    struct bus_device dev = {
        .name         = "env",
        .class        = BUS_CLASS_ENV,
        .version      = 1,
        .ops          = &env_bus_ops,
        .private_data = e,
        .owner_id     = 3,
    };
    bus_register(&dev);
    bus_test("env");
}
