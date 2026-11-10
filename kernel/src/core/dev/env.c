#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <lib/mem.h>
#include <lib/string.h>
#include <core/arch/cpu.h>
#include <core/arch/heap.h>
#include <core/dev/timer.h>
#include <core/dev/env.h>
#include <core/mask.h>
#include <core/panic.h>
#include <core/klog.h>
#include <core/bus.h>

/* entry */
struct env_entry {
    char         *ns;           /* BLOCK BASE: ns, name and value are three
                                 * slices of ONE allocation; kfree(ns) frees all */
    char         *name;         /* points inside the ns block */
    char         *value;        /* points inside the ns block */

    uint32_t     ns_len;
    uint32_t     name_len;
    uint32_t     value_len;

    uint32_t     access_remaining;   /* 0 = unlimited */
    uint64_t     expires_at;         /* ticks; 0 = no time budget */
    uint64_t     last_access;        /* ticks at last read or write */
    uint8_t      importance;         /* higher = more valuable */
    uint32_t     mask;               /* opaque, caller-defined classification bits */

    bool         live;
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
    size_t               capacity;           /* number of slots */
    size_t               count;              /* live entries (expired-but-unswept included) */
    size_t               used_bytes;         /* sum of ns_len + name_len + value_len */
    size_t               max_bytes;

    /* Exact mask index. Maintained incrementally on every set / reclaim, so it
     * never needs a rescan and never goes stale:
     *   bit_cnt[b]     number of entries with mask bit b set
     *   active_mask_or bit b is set <=> bit_cnt[b] > 0   (the exact OR of all masks)
     *   owner_cnt[o]   number of entries whose OWNER field == o
     *   type_cnt[t]    number of entries whose TYPE  field == t
     * Entries that have expired but not yet been swept are still counted, so a
     * ZERO here proves "nothing can match" (the O(1) short-circuit) while a
     * non-zero value only means "a scan is needed". The index is a filter,
     * never an answer; it can cost a scan but can never produce a wrong result. */
    uint32_t             active_mask_or;
    uint32_t             bit_cnt[32];
    uint32_t             owner_cnt[256];
    uint32_t             type_cnt[16];

    const struct timer_ops *timer;        /* NULL if no timer device */
    int                  sweep_id;          /* timer task id, or -1 */
    size_t               sweep_pos;         /* sweeper resume cursor (slot index) */

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

/* MASK INDEX */
static void env_mask_account(struct env_device *e, uint32_t mask) {
    e->owner_cnt[env_mask_owner(mask)]++;
    e->type_cnt[env_mask_type(mask)]++;
    for (uint32_t m = mask; m != 0; m &= m - 1) {
        unsigned b = (unsigned)__builtin_ctz(m);
        if (e->bit_cnt[b]++ == 0)
            e->active_mask_or |= (1u << b);
    }
}

static void env_mask_unaccount(struct env_device *e, uint32_t mask) {
    uint8_t o = env_mask_owner(mask), t = env_mask_type(mask);
    if (e->owner_cnt[o] > 0) e->owner_cnt[o]--;
    if (e->type_cnt[t]  > 0) e->type_cnt[t]--;
    for (uint32_t m = mask; m != 0; m &= m - 1) {
        unsigned b = (unsigned)__builtin_ctz(m);
        if (e->bit_cnt[b] > 0 && --e->bit_cnt[b] == 0)
            e->active_mask_or &= ~(1u << b);
    }
}

/* True only if NO entry can satisfy (mask & care) == want. A "false" means
 * "maybe": the caller must scan. `want` must be a subset of `care`. */
static bool env_match_impossible(const struct env_device *e, uint32_t care, uint32_t want) {
    if ((e->active_mask_or & want) != want) return true;
    if ((care & ENV_MASK_OWNER_MASK) == ENV_MASK_OWNER_MASK &&
        e->owner_cnt[(want >> ENV_MASK_OWNER_SHIFT) & 0xFFu] == 0) return true;
    if ((care & ENV_MASK_TYPE_MASK) == ENV_MASK_TYPE_MASK &&
        e->type_cnt[(want >> ENV_MASK_TYPE_SHIFT) & 0xFu] == 0) return true;
    return false;
}

/* CORE HELPERS */
static inline size_t env_footprint(const struct env_entry *s) {
    return (size_t)s->ns_len + s->name_len + s->value_len;
}

static inline uint64_t env_now(struct env_device *e) {
    return (e->timer != NULL) ? e->timer->ticks() : 0;
}

/* One timer cross-read per OPERATION, hoisted by every caller; never call this in a per-entry loop. */
static inline bool env_expired(uint64_t now, const struct env_entry *slot) {
    if (!slot->live) return true;
    if (slot->expires_at == 0) return false;
    return now >= slot->expires_at;
}

/* SECRET entries are hidden from any enumeration whose `care` ignores the bit. */
static inline bool env_visible(const struct env_entry *s, uint32_t care) {
    return !(s->mask & ENV_FLAG_SECRET) || (care & ENV_FLAG_SECRET);
}

/* Raw lookup: finds the live entry for a key whether or not it has expired.
 * There is at most one live entry per key (env_set reclaims an expired
 * same-key entry before inserting). */
static struct env_entry *env_lookup(struct env_device *e,
                                    const char *ns, size_t ns_len,
                                    const char *name, size_t name_len) {
    for (size_t i = 0; i < e->capacity; i++) {
        struct env_entry *s = &e->entries[i];
        if (!s->live) continue;
        if (s->ns_len != ns_len) continue;
        if (s->name_len != name_len) continue;
        if (memcmp(s->ns, ns, ns_len) != 0) continue;
        if (memcmp(s->name, name, name_len) != 0) continue;
        return s;
    }
    return NULL;
}

/* Lookup as seen by readers: expired entries read as absent. */
static struct env_entry *env_find(struct env_device *e,
                                  const char *ns, size_t ns_len,
                                  const char *name, size_t name_len,
                                  uint64_t now) {
    struct env_entry *s = env_lookup(e, ns, ns_len, name, name_len);
    return (s != NULL && !env_expired(now, s)) ? s : NULL;
}

static struct env_entry *env_find_free_slot(struct env_device *e) {
    for (size_t i = 0; i < e->capacity; i++) {
        if (!e->entries[i].live) return &e->entries[i];
    }
    return NULL;
}

/* Single block staging: one heap header instead of three,
 * one alloc/free pair instead of three pairs, contiguous for the memcmp scan. */
static int env_stage_strings(const char *ns, size_t ns_len,
                             const char *name, size_t name_len,
                             const char *value, size_t value_len,
                             char **out_ns, char **out_name, char **out_value) {
    char *block = kmalloc(ns_len + name_len + value_len + 3);
    if (block == NULL)
        return ENV_NO_SPACE;   /* heap exhaustion: nearest errno we carry */

    char *p = block;
    memcpy(p, ns, ns_len);       p[ns_len] = '\0';       *out_ns = p;    p += ns_len + 1;
    memcpy(p, name, name_len);   p[name_len] = '\0';     *out_name = p;  p += name_len + 1;
    memcpy(p, value, value_len); p[value_len] = '\0';    *out_value = p;
    return ENV_OK;
}

/* RECLAMATION */
static void env_reclaim(struct env_device *e, struct env_entry *slot,
                        enum env_reclaim_reason reason) {
    if (!slot->live) return;

    size_t freed = env_footprint(slot);
    if (freed <= e->used_bytes) e->used_bytes -= freed;
    else e->used_bytes = 0;

    env_mask_unaccount(e, slot->mask);

    kfree(slot->ns);                      /* single block: frees ns + name + value */
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
    case ENV_RECLAIM_ACCESS:    e->n_reclaim_access++;    break;
    case ENV_RECLAIM_TIME:      e->n_reclaim_time++;      break;
    case ENV_RECLAIM_SPACE:     e->n_reclaim_space++;     break;
    case ENV_RECLAIM_EXPLICIT:  e->n_reclaim_explicit++;  break;
    }
}

/* EVICTION
 *
 * Eligibility is one predicate, shared by the feasibility check and by victim selection,
 * so the question "can we make room?" and "who gets evicted?" can never disagree.
 *   - expired entries are always eligible (even PINNED: the TTL already won)
 *   - PINNED live entries never are
 *   - a live entry is eligible only if it is no more valuable than the write
 *     that wants its space (importance <= incoming)
 */
static inline bool env_evictable(const struct env_entry *s, uint64_t now,
                                 const struct env_entry *exclude, uint8_t incoming) {
    if (!s->live || s == exclude) return false;
    if (env_expired(now, s)) return true;
    if (s->mask & ENV_FLAG_PINNED) return false;
    return s->importance <= incoming;
}

/* Preference: expired, then lowest importance, then CACHEABLE, then LRU, then name. */
static struct env_entry *env_pick_victim(struct env_device *e, uint64_t now,
                                         const struct env_entry *exclude, uint8_t incoming) {
    struct env_entry *victim = NULL;
    for (size_t i = 0; i < e->capacity; i++) {
        struct env_entry *s = &e->entries[i];
        if (!env_evictable(s, now, exclude, incoming)) continue;
        if (victim == NULL) { victim = s; continue; }

        bool s_exp = env_expired(now, s), v_exp = env_expired(now, victim);
        if (s_exp != v_exp) { if (s_exp) victim = s; continue; }
        if (s->importance < victim->importance) { victim = s; continue; }
        if (s->importance > victim->importance) continue;
        bool s_c = (s->mask & ENV_FLAG_CACHEABLE) != 0;
        bool v_c = (victim->mask & ENV_FLAG_CACHEABLE) != 0;
        if (s_c != v_c) { if (s_c) victim = s; continue; }
        if (s->last_access < victim->last_access) { victim = s; continue; }
        if (s->last_access > victim->last_access) continue;
        if (strcmp(s->name, victim->name) < 0) victim = s;
    }
    return victim;
}

/* All-or-nothing: first prove that enough eligible space exists, and only then evict.
 * A set() that cannot succeed therefore never costs anyone an entry.
 */
static bool env_make_room(struct env_device *e, uint64_t now, size_t footprint,
                          const struct env_entry *existing, uint8_t incoming) {
    size_t credit = (existing != NULL) ? env_footprint(existing) : 0;
    if (credit > e->used_bytes) credit = e->used_bytes;

    size_t projected = e->used_bytes - credit + footprint;
    size_t need = (projected > e->max_bytes) ? projected - e->max_bytes : 0;
    bool have_slot = (existing != NULL) || (env_find_free_slot(e) != NULL);

    if (have_slot && need == 0) return true;

    size_t avail_bytes = 0, avail_cnt = 0;
    for (size_t i = 0; i < e->capacity; i++) {
        const struct env_entry *s = &e->entries[i];
        if (!env_evictable(s, now, existing, incoming)) continue;
        avail_bytes += env_footprint(s);
        avail_cnt++;
    }
    if (avail_bytes < need) return false;
    if (!have_slot && avail_cnt == 0) return false;

    size_t freed = 0;
    while (!have_slot || freed < need) {
        struct env_entry *victim = env_pick_victim(e, now, existing, incoming);
        if (victim == NULL) return false;      /* unreachable after the check above */
        freed += env_footprint(victim);
        env_reclaim(e, victim, ENV_RECLAIM_SPACE);
        have_slot = true;
    }
    return true;
}

/* OPS */
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
    if (footprint > e->max_bytes) return ENV_NO_SPACE;

    /* one timer read for the whole operation: expiry, TTL and bookkeeping all key off the same now. */
    uint64_t now = env_now(e);

    /* An expired entry under the same key is garbage: collect it now so the key has exactly one live slot.
     * This is not a side effect of a failed set(), the entry was already unreadable. */
    struct env_entry *existing = env_lookup(e, ns, ns_len, name, name_len);
    if (existing != NULL && env_expired(now, existing)) {
        env_reclaim(e, existing, ENV_RECLAIM_TIME);
        existing = NULL;
    }

    /* Policy validation, before anything is allocated or evicted.
     * These test the RAW mask against the unshifted ENV_FLAG_* constants. */
    if (existing != NULL) {
        uint32_t old = existing->mask;
        if (old & ENV_FLAG_READONLY)
            return ENV_READONLY;
        if ((old & ENV_FLAG_PINNED) && env_mask_owner(old) != env_mask_owner(mask))
            return ENV_READONLY;
    }

    /* Stage the new strings. Heap exhaustion fails here, before any eviction. */
    char *n_ns = NULL, *n_name = NULL, *n_value = NULL;
    int st = env_stage_strings(ns, ns_len, name, name_len, value, value_len,
                               &n_ns, &n_name, &n_value);
    if (st != ENV_OK) return st;

    /* Make room (all-or-nothing). */
    if (!env_make_room(e, now, footprint, existing, importance)) {
        kfree(n_ns);
        return ENV_NO_SPACE;
    }

    /* Commit. Nothing below can fail except the can't-happen slot check. */
    struct env_entry *slot = existing;
    if (slot == NULL) {
        slot = env_find_free_slot(e);
        if (slot == NULL) {
            kfree(n_ns);
            return ENV_INTERNAL;
        }
        e->count++;
    } else {
        size_t old = env_footprint(slot);
        e->used_bytes = (old <= e->used_bytes) ? e->used_bytes - old : 0;
        env_mask_unaccount(e, slot->mask);
        kfree(slot->ns);                  /* frees the whole old block */
    }

    slot->ns = n_ns;
    slot->name = n_name;
    slot->value = n_value;
    slot->ns_len = (uint32_t)ns_len;
    slot->name_len = (uint32_t)name_len;
    slot->value_len = (uint32_t)value_len;

    slot->access_remaining = access_budget;
    /* Saturating TTL: now + ttl must not wrap into the past. */
    slot->expires_at = (ttl_ticks == 0) ? 0
                     : (ttl_ticks > UINT64_MAX - now) ? UINT64_MAX
                     : now + ttl_ticks;
    slot->last_access = now;
    slot->importance = importance;
    slot->mask = mask;
    slot->live = true;

    env_mask_account(e, mask);
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

    uint64_t now = env_now(e);
    struct env_entry *s = env_find(e, ns, ns_len, name, name_len, now);
    if (s == NULL) return ENV_NOT_FOUND;

    if (len_out != NULL) *len_out = s->value_len;
    if (buf_max < (size_t)s->value_len + 1) return ENV_BUFFER_TOO_SMALL;

    memcpy(buf, s->value, s->value_len);
    buf[s->value_len] = '\0';
    s->last_access = now;

    if (!consume) {
        e->n_peek++;
        return ENV_OK;
    }

    e->n_get++;
    if (s->access_remaining == 1)
        env_reclaim(e, s, ENV_RECLAIM_ACCESS);
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

    uint64_t now = env_now(e);
    struct env_entry *s = env_find(e, ns, strlen(ns), name, strlen(name), now);
    if (s == NULL) return ENV_NOT_FOUND;

    env_reclaim(e, s, ENV_RECLAIM_EXPLICIT);
    e->n_unset++;
    return ENV_OK;
}

static int env_is_exist(const char *ns, const char *name) {
    struct env_device *e = g_env;
    if (e == NULL) return ENV_NO_DEVICE;
    if (ns == NULL || name == NULL) return ENV_INVALID_ARGS;

    uint64_t now = env_now(e);
    struct env_entry *s = env_find(e, ns, strlen(ns), name, strlen(name), now);
    return (s != NULL) ? ENV_OK : ENV_NOT_FOUND;
}

static int env_stat(const char *ns, const char *name, struct env_stat *out) {
    struct env_device *e = g_env;
    if (e == NULL) return ENV_NO_DEVICE;
    if (ns == NULL || name == NULL || out == NULL) return ENV_INVALID_ARGS;

    uint64_t now = env_now(e);
    struct env_entry *s = env_find(e, ns, strlen(ns), name, strlen(name), now);
    if (s == NULL) return ENV_NOT_FOUND;

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

/* ENUMERATION
 *
 * One engine behind list / list_all / list_by_mask / list_match:
 *   ns_filter  NULL = all namespaces
 *   care/want  an entry matches when (mask & care) == want; care == 0 matches all
 *   SECRET     entries are skipped unless `care` includes ENV_FLAG_SECRET
 *   ns_out     may be NULL (list() only reports names)
 */
static void env_emit(const struct env_entry *s,
                     char *ns_out, size_t ns_max,
                     char *name_out, size_t name_max) {
    if (ns_out != NULL) {
        size_t ns_copy = s->ns_len;
        if (ns_copy >= ns_max) ns_copy = ns_max - 1;
        memcpy(ns_out, s->ns, ns_copy);
        ns_out[ns_copy] = '\0';
    }
    size_t name_copy = s->name_len;
    if (name_copy >= name_max) name_copy = name_max - 1;
    memcpy(name_out, s->name, name_copy);
    name_out[name_copy] = '\0';
}

static int env_enum(struct env_device *e, const char *ns_filter,
                    uint32_t care, uint32_t want, size_t *iter,
                    char *ns_out, size_t ns_max,
                    char *name_out, size_t name_max) {
    size_t ns_len = (ns_filter != NULL) ? strlen(ns_filter) : 0;

    if (care != 0 && env_match_impossible(e, care, want)) {
        *iter = e->capacity;
        return ENV_NOT_FOUND;
    }

    uint64_t now = env_now(e);
    for (size_t i = *iter; i < e->capacity; i++) {
        struct env_entry *s = &e->entries[i];
        if (!s->live) continue;
        if (env_expired(now, s)) continue;
        if (!env_visible(s, care)) continue;
        if (ns_filter != NULL) {
            if (s->ns_len != ns_len) continue;
            if (memcmp(s->ns, ns_filter, ns_len) != 0) continue;
        }
        if ((s->mask & care) != want) continue;

        env_emit(s, ns_out, ns_max, name_out, name_max);
        *iter = i + 1;
        return ENV_OK;
    }
    *iter = e->capacity;
    return ENV_NOT_FOUND;
}

static int env_count(struct env_device *e, uint32_t care, uint32_t want, size_t *count_out) {
    if (care != 0 && env_match_impossible(e, care, want)) {
        *count_out = 0;
        return ENV_OK;
    }
    uint64_t now = env_now(e);
    size_t n = 0;
    for (size_t i = 0; i < e->capacity; i++) {
        struct env_entry *s = &e->entries[i];
        if (!s->live || env_expired(now, s)) continue;
        if (!env_visible(s, care)) continue;
        if ((s->mask & care) == want) n++;
    }
    *count_out = n;
    return ENV_OK;
}

static int env_list(const char *ns, size_t *iter,
                    char *name_out, size_t name_max) {
    struct env_device *e = g_env;
    if (e == NULL) return ENV_NO_DEVICE;
    if (ns == NULL || iter == NULL || name_out == NULL) return ENV_INVALID_ARGS;
    if (name_max == 0) return ENV_INVALID_ARGS;
    return env_enum(e, ns, 0, 0, iter, NULL, 0, name_out, name_max);
}

static int env_list_all(size_t *iter,
                        char *ns_out, size_t ns_max,
                        char *name_out, size_t name_max) {
    struct env_device *e = g_env;
    if (e == NULL) return ENV_NO_DEVICE;
    if (iter == NULL || ns_out == NULL || name_out == NULL) return ENV_INVALID_ARGS;
    if (ns_max == 0 || name_max == 0) return ENV_INVALID_ARGS;
    return env_enum(e, NULL, 0, 0, iter, ns_out, ns_max, name_out, name_max);
}

static int env_list_match(uint32_t care, uint32_t want, size_t *iter,
                          char *ns_out, size_t ns_max,
                          char *name_out, size_t name_max) {
    struct env_device *e = g_env;
    if (e == NULL) return ENV_NO_DEVICE;
    if (care == 0 || (want & ~care) != 0) return ENV_INVALID_ARGS;
    if (iter == NULL || ns_out == NULL || name_out == NULL) return ENV_INVALID_ARGS;
    if (ns_max == 0 || name_max == 0) return ENV_INVALID_ARGS;
    return env_enum(e, NULL, care, want, iter, ns_out, ns_max, name_out, name_max);
}

static int env_count_match(uint32_t care, uint32_t want, size_t *count_out) {
    struct env_device *e = g_env;
    if (e == NULL) return ENV_NO_DEVICE;
    if (care == 0 || (want & ~care) != 0 || count_out == NULL) return ENV_INVALID_ARGS;
    return env_count(e, care, want, count_out);
}

/* Legacy subset-match entry points: care == want == query_mask. */
static int env_list_by_mask(uint32_t query_mask, size_t *iter,
                            char *ns_out, size_t ns_max,
                            char *name_out, size_t name_max) {
    if (query_mask == 0) return (g_env == NULL) ? ENV_NO_DEVICE : ENV_INVALID_ARGS;
    return env_list_match(query_mask, query_mask, iter, ns_out, ns_max, name_out, name_max);
}

static int env_count_by_mask(uint32_t query_mask, size_t *count_out) {
    if (query_mask == 0) return (g_env == NULL) ? ENV_NO_DEVICE : ENV_INVALID_ARGS;
    return env_count_match(query_mask, query_mask, count_out);
}

/* Core-only: ignores READONLY / PINNED by design. Never expose through the proxy. */
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

/* Asynchronous sweeper: bounded batch per tick, resume cursor, self-rescheduling.
 * Reads are always exact (expired entries read as absent) whether or not the
 * sweeper has run; it only returns the memory and the slot. */
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
        klog_warning("env_sweep(): environment timer could not reschedule; falling back to lazy expiry only\n");
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

    g_env = NULL;
    kfree(e->entries);
    kfree(e);
}

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
    .list_match     = env_list_match,
    .count_match    = env_count_match,
};

void env_init(size_t max_entries, size_t max_bytes) {
    if (g_env != NULL) {
        klog_warning("env_init() called twice; ignoring\n");
        return;
    }
    if (max_entries == 0) max_entries = ENV_DEFAULT_MAX_ENTRIES;
    if (max_bytes   == 0) max_bytes   = ENV_DEFAULT_MAX_BYTES;
    if (max_entries > SIZE_MAX / sizeof(struct env_entry))
        PANIC("env: slot table size overflow");

    struct env_device *e = kzalloc(sizeof *e);
    if (e == NULL) PANIC("env: cannot allocate device");

    e->entries = kzalloc(max_entries * sizeof *e->entries);
    if (e->entries == NULL) PANIC("env: cannot allocate slot table");

    e->capacity       = max_entries;
    e->max_bytes      = max_bytes;
    e->active_mask_or = 0;
    e->sweep_id       = -1;
    e->sweep_pos      = 0;

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
        .owner_id     = BUS_ENV_ID,
    };
    if (bus_register(&dev) != 0)
        klog_warning("env: bus registration failed\n");
    bus_test("env");
}
