#include "klog.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <lib/mem.h>
#include <lib/string.h>
#include <core/cpu.h>
#include <core/panic.h>
#include <core/klog.h>
#include <core/heap.h>
#include <core/timer.h>
#include <core/bus.h>
#include <core/env.h>

/* entry */

struct env_entry {
    char    *ns;                /* owned; NULL when slot is free */
    char    *name;              /* owned */
    char    *value;             /* owned */

    uint32_t ns_len;
    uint32_t name_len;
    uint32_t value_len;

    uint32_t access_remaining;  /* 0 = unlimited */
    uint64_t expires_at;        /* ticks; 0 = no time budget */
    uint64_t last_access;       /* ticks at last read or write */
    uint8_t  importance;

    bool     live;
};

/* device state */

struct env_device {
    struct env_entry *entries;
    size_t            capacity;         /* number of slots */
    size_t            count;            /* live entries */
    size_t            used_bytes;       /* sum of ns_len + name_len + value_len */
    size_t            max_bytes;

    const struct timer_ops *timer;      /* NULL if no timer device */
    int               sweep_id;         /* timer task id, or -1 */

    /* cumulative counters, for stats and diagnostics */
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

static void env_reclaim(struct env_device *e, struct env_entry *slot, int reason);
static void env_sweep(void *arg);

/* helpers */

static uint64_t env_now(struct env_device *e) {
    return (e->timer != NULL) ? e->timer->ticks() : 0;
}

/* Rule 4: logically expired == absent.
 * An entry with expires_at == 0 has no time budget and never expires. */
static bool env_expired(struct env_device *e, const struct env_entry *slot) {
    if (!slot->live) return true;
    if (slot->expires_at == 0) return false;
    return env_now(e) >= slot->expires_at;
}

// unused
/* static bool env_logically_live(struct env_device *e, const struct env_entry *slot) {
    if (!slot->live) return false;
    if (env_expired(e, slot)) return false;
    // access budget: 0 == unlimited; otherwise, dead at 0
    return true;   // slot with access_remaining == 0 has already been reclaimed
}
*/

static struct env_entry *env_find(struct env_device *e,
                                  const char *ns, size_t ns_len,
                                  const char *name, size_t name_len) {
    for (size_t i = 0; i < e->capacity; i++) {
        struct env_entry *s = &e->entries[i];
        if (!s->live) continue;
        if (env_expired(e, s)) continue;   /* treat as absent; sweeper will reap */
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

/* Reclaim one slot. Bounded, allocation-free, safe to call from the
 * access path. reason is for stats only. */
static void env_reclaim(struct env_device *e, struct env_entry *slot, int reason) {
    if (!slot->live) return;

    size_t freed = slot->ns_len + slot->name_len + slot->value_len;
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
    slot->live = false;

    if (e->count > 0) e->count--;

    switch (reason) {
    case 0: e->n_reclaim_access++;   break;
    case 1: e->n_reclaim_time++;     break;
    case 2: e->n_reclaim_space++;    break;
    case 3: e->n_reclaim_explicit++; break;
    }
}

/* Deterministic victim selection under space pressure:
 * max importance, tie-break min last_access, tie-break name lexicographic.
 * Returns NULL if every live slot is protected (currently impossible,
 * since we have no pin mechanism, but the shape is here for later). */
static struct env_entry *env_pick_victim(struct env_device *e) {
    struct env_entry *victim = NULL;
    for (size_t i = 0; i < e->capacity; i++) {
        struct env_entry *s = &e->entries[i];
        if (!s->live) continue;
        if (victim == NULL) { victim = s; continue; }
        if (s->importance > victim->importance) { victim = s; continue; }
        if (s->importance < victim->importance) continue;
        if (s->last_access < victim->last_access) { victim = s; continue; }
        if (s->last_access > victim->last_access) continue;
        int c = strcmp(s->name, victim->name);
        if (c < 0) victim = s;
    }
    return victim;
}

/* Make room for a new entry of the given footprint. May evict multiple entries.
 * Returns true if room was made, false if not. */
static bool env_make_room(struct env_device *e, size_t footprint) {
    /* Rule 4 says expired entries are already absent,
     * but they may still be occupying slots.
     * Reap them first: it's free, and it avoids evicting a live entry to make room for a new one. */
    for (size_t i = 0; i < e->capacity; i++) {
        if (e->entries[i].live && env_expired(e, &e->entries[i]))
            env_reclaim(e, &e->entries[i], 1);
    }

    for (;;) {
        bool need_slot  = (env_find_free_slot(e) == NULL);
        bool need_bytes = (footprint > 0 && e->used_bytes + footprint > e->max_bytes);
        if (!need_slot && !need_bytes) return true;

        struct env_entry *victim = env_pick_victim(e);
        if (victim == NULL) return false;   /* nothing left to evict */
        env_reclaim(e, victim, 2);
    }
}

/* ops */

static int env_set(const char *ns, const char *name, const char *value,
                   uint32_t access_budget, uint64_t ttl_ticks,
                   uint8_t importance) {
    struct env_device *e = g_env;
    if (e == NULL) return ENV_NO_DEVICE;
    if (ns == NULL || name == NULL || value == NULL) return ENV_INVALID_ARGS;

    size_t ns_len    = strlen(ns);
    size_t name_len  = strlen(name);
    size_t value_len = strlen(value);
    if (ns_len == 0 || name_len == 0) return ENV_INVALID_ARGS;
    if (name_len > ENV_NAME_MAX) return ENV_INVALID_ARGS;
    if (value_len > ENV_VALUE_MAX) return ENV_INVALID_ARGS;
    if (ns_len > ENV_NAME_MAX) return ENV_INVALID_ARGS;   /* same bound as name */

    if (importance > ENV_IMPORTANCE_MAX) importance = ENV_IMPORTANCE_MAX;

    /* Rule 1 + 2: full declaration, atomic replace.
     * If an entry exists at this key, reclaim it first. That frees its
     * bytes and its slot, then we write the fresh declaration. */
    struct env_entry *existing = env_find(e, ns, ns_len, name, name_len);
    if (existing != NULL) {
        env_reclaim(e, existing, 3);
    }

    size_t footprint = ns_len + name_len + value_len;
    if (!env_make_room(e, footprint)) return ENV_NO_SPACE;

    struct env_entry *slot = env_find_free_slot(e);
    if (slot == NULL) return ENV_INTERNAL;

    slot->ns = kmalloc(ns_len + 1);
    slot->name = kmalloc(name_len + 1);
    slot->value = kmalloc(value_len + 1);
    if (slot->ns == NULL || slot->name == NULL || slot->value == NULL) {
        kfree(slot->ns); kfree(slot->name); kfree(slot->value);
        slot->ns = slot->name = slot->value = NULL;
        return ENV_NO_SPACE;
    }
    memcpy(slot->ns, ns, ns_len); slot->ns[ns_len] = '\0';
    memcpy(slot->name, name, name_len); slot->name[name_len] = '\0';
    memcpy(slot->value, value, value_len); slot->value[value_len] = '\0';

    slot->ns_len = (uint32_t)ns_len;
    slot->name_len = (uint32_t)name_len;
    slot->value_len = (uint32_t)value_len;

    slot->access_remaining = access_budget;   /* 0 = unlimited */
    slot->expires_at = (ttl_ticks == 0) ? 0 : env_now(e) + ttl_ticks;
    slot->last_access = env_now(e);
    slot->importance = importance;
    slot->live = true;

    e->count++;
    e->used_bytes += footprint;
    e->n_set++;
    return ENV_OK;
}

static int env_get_common(const char *ns, const char *name,
                          const char **out, bool consume) {
    struct env_device *e = g_env;
    if (e == NULL) return ENV_NO_DEVICE;
    if (ns == NULL || name == NULL || out == NULL) return ENV_INVALID_ARGS;

    size_t ns_len = strlen(ns), name_len = strlen(name);
    if (ns_len == 0 || name_len == 0) return ENV_INVALID_ARGS;

    /* The find() already filters expired entries,
     * so ENV_EXPIRED never appears here: rule 4 collapses it into ENV_NOT_FOUND.
     * The ENV_EXPIRED code exists for callers who want to distinguish:
     * "existed and died" from "never existed" through other means. */
    struct env_entry *s = env_find(e, ns, ns_len, name, name_len);
    if (s == NULL) return ENV_NOT_FOUND;

    if (consume) {
        if (s->access_remaining == 0) {
            /* unlimited; nothing to do */
        } else if (s->access_remaining == 1) {
            /* this read is the last one; hand out the value, then reclaim */
            *out = s->value;
            s->last_access = env_now(e);
            e->n_get++;
            /* reclaim after the caller has the pointer;
             * caller is responsible for copying before its next mutating call,
             * which is already the documented contract for get()/peek() */
            env_reclaim(e, s, 0);
            return ENV_OK;
        } else {
            s->access_remaining--;
        }
        e->n_get++;
    } else {
        e->n_peek++;
    }

    s->last_access = env_now(e);
    *out = s->value;
    return ENV_OK;
}

static int env_get(const char *ns, const char *name, const char **out) {
    return env_get_common(ns, name, out, true);
}

static int env_peek(const char *ns, const char *name, const char **out) {
    return env_get_common(ns, name, out, false);
}

static int env_unset(const char *ns, const char *name) {
    struct env_device *e = g_env;
    if (e == NULL) return ENV_NO_DEVICE;
    if (ns == NULL || name == NULL) return ENV_INVALID_ARGS;

    struct env_entry *s = env_find(e, ns, strlen(ns), name, strlen(name));
    if (s == NULL) return ENV_NOT_FOUND;

    env_reclaim(e, s, 3);
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

static int env_list(const char *ns, size_t *iter,
                    char *name_out, size_t name_max) {
    struct env_device *e = g_env;
    if (e == NULL) return ENV_NO_DEVICE;
    if (ns == NULL || iter == NULL || name_out == NULL) return ENV_INVALID_ARGS;
    if (name_max == 0) return ENV_INVALID_ARGS;

    size_t ns_len = strlen(ns);
    size_t seen = 0;

    for (size_t i = 0; i < e->capacity; i++) {
        struct env_entry *s = &e->entries[i];
        if (!s->live) continue;
        if (env_expired(e, s)) continue;
        if (s->ns_len != ns_len) continue;
        if (memcmp(s->ns, ns, ns_len) != 0) continue;

        if (seen < *iter) { seen++; continue; }

        size_t copy = s->name_len;
        if (copy >= name_max) copy = name_max - 1;
        memcpy(name_out, s->name, copy);
        name_out[copy] = '\0';
        (*iter)++;
        return ENV_OK;
    }
    return ENV_NOT_FOUND;   /* iteration complete */
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
        env_reclaim(e, s, 3);
    }
    return ENV_OK;
}

/* sweeper  */

static void env_sweep(void *arg) {
    struct env_device *e = arg;
    if (e == NULL || e->timer == NULL) return;

    uint64_t now = e->timer->ticks();
    size_t examined = 0;

    for (size_t i = 0; i < e->capacity && examined < ENV_SWEEP_BATCH; i++) {
        struct env_entry *s = &e->entries[i];
        if (!s->live) continue;
        examined++;
        if (s->expires_at != 0 && now >= s->expires_at)
            env_reclaim(e, s, 1);
    }

    e->n_sweeps++;
    e->sweep_id = e->timer->after(ENV_SWEEP_PERIOD_MS, env_sweep, e);
}

/* bus registration */

static const struct env_ops env_bus_ops = {
    .set            = env_set,
    .get            = env_get,
    .peek           = env_peek,
    .unset          = env_unset,
    .is_exist       = env_is_exist,
    .list           = env_list,
    .drop_namespace = env_drop_namespace,
};

const struct env_ops *bus_env(void) {
    const struct bus_device *d = bus_find_class(BUS_CLASS_ENV, 0);
    return (d != NULL) ? (const struct env_ops *)d->ops : NULL;
}

/* init */

void env_init(size_t max_entries, size_t max_bytes) {
    if (max_entries == 0) max_entries = ENV_DEFAULT_MAX_ENTRIES;
    if (max_bytes   == 0) max_bytes   = ENV_DEFAULT_MAX_BYTES;

    struct env_device *e = kzalloc(sizeof *e);
    if (e == NULL) PANIC("env: cannot allocate device");

    e->entries = kzalloc(max_entries * sizeof *e->entries);
    if (e->entries == NULL) PANIC("env: cannot allocate slot table");

    e->capacity   = max_entries;
    e->count      = 0;
    e->used_bytes = 0;
    e->max_bytes  = max_bytes;
    e->sweep_id   = -1;

    g_env = e;

    /* Resolve the timer, if any. The device works without one. */
    e->timer = bus_timer();
    if (e->timer != NULL) {
        e->sweep_id = e->timer->after(ENV_SWEEP_PERIOD_MS, env_sweep, e);
        if (e->sweep_id < 0)
            klog_warning("Environment sweep timer not enabled\n");
    }

    struct bus_device dev = {
        .name         = "env",
        .class        = BUS_CLASS_ENV,
        .version      = 1,
        .ops          = &env_bus_ops,
        .private_data = e,
    };
    bus_register(&dev);
    bus_test("env");
    klog_env("→ env_init() entries=%zu bytes=%zu timer=%s\n",
         max_entries, max_bytes,
         e->timer ? "true" : "false");
}

/* Called from a shutdown path, or from a context that wants to fully tear down the device.
 * Cancels the sweeper, frees every entry, frees the slot table, clears g_env. */
void env_destroy(void) {
    struct env_device *e = g_env;
    if (e == NULL) return;

    if (e->timer != NULL && e->sweep_id >= 0)
        e->timer->cancel(e->sweep_id);
    e->sweep_id = -1;

    for (size_t i = 0; i < e->capacity; i++) {
        if (e->entries[i].live) env_reclaim(e, &e->entries[i], 3);
    }

    kfree(e->entries);
    kfree(e);
    g_env = NULL;
}
