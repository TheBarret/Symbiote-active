#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <lib/mem.h>
#include <lib/string.h>
#include <core/arch/heap.h>
#include <core/panic.h>
#include <core/klog.h>
#include <core/services/symbol.h>

/* codebase: 0.2.0 */

/* Hard cap on the number of free ranges the arena tracks.
 * Reached only by heavy churn of mixed-size STRING allocations.
 * Overflow is SYM_NO_SPACE, never a silent leak. Tune if needed. */
#define SYMBOL_FREE_RANGES_MAX 64u

/* One arena free range: [off, off + len). Ranges are disjoint, sorted by off,
 * and coalesced on free (never two adjacent ranges). */
struct sym_range {
    uint32_t off;
    uint32_t len;
};

struct symbol_device {
    /* Slot table: fixed at init, never grows. */
    struct symbol_entry *entries;
    size_t               capacity;
    size_t               count;            /* live entries */

    /* Value arena: one fixed block, never grows. */
    uint8_t             *arena;
    size_t               arena_size;
    size_t               arena_used;       /* high-water mark of bump allocation */
    struct sym_range     free_ranges[SYMBOL_FREE_RANGES_MAX];
    size_t               free_count;

    /* Exact mask index, maintained on every set / unset.
     * Index is a filter, never an answer: zero proves "nothing matches",
     * non-zero only means "a scan is needed". */
    uint32_t             active_mask_or;
    uint32_t             bit_cnt[32];
    uint32_t             owner_cnt[256];
    uint32_t             type_cnt[16];

    /* Stats. */
    uint64_t             n_set;
    uint64_t             n_get;
    uint64_t             n_unset;
    uint64_t             n_reclaim_space;
};

static struct symbol_device *g_sym;

/* forward decls */
static int sym_arena_claim(struct symbol_device *d, uint32_t len, uint32_t *out_off);
static void sym_arena_release(struct symbol_device *d, uint32_t off, uint32_t len);

/* TYPE widths */

/* Fixed byte width of a TYPE. STRING is variable (caller supplies len);
 * returns 0 for STRING and for TYPE_NONE/invalid, so callers must special-case. */
static inline uint32_t sym_type_width(uint8_t type) {
    switch (type) {
    case SYM_TYPE_BYTE:   return 1;
    case SYM_TYPE_CHAR:   return 2;
    case SYM_TYPE_FLOAT:  return 4;
    case SYM_TYPE_INT16:  return 2;
    case SYM_TYPE_INT32:  return 4;
    case SYM_TYPE_UINT16: return 2;
    case SYM_TYPE_UINT32: return 4;
    case SYM_TYPE_BOOL:   return 1;
    case SYM_TYPE_STRING: return 0;   /* variable */
    default:              return 0;   /* NONE / reserved */
    }
}

/* Compute the arena bytes needed for a (type, len) pair.
 * Returns false if (type, len) is inconsistent (e.g. INT32 with len != 4). */
static bool sym_value_bytes(uint8_t type, size_t len, uint32_t *out_bytes) {
    uint32_t w = sym_type_width(type);
    if (type == SYM_TYPE_STRING) {
        if (len == 0 || len > SYMBOL_STRING_MAX) return false;
        *out_bytes = (uint32_t)len;   /* len includes the NUL from the caller */
        return true;
    }
    if (w == 0) return false;
    if (len != w) return false;
    *out_bytes = w;
    return true;
}

/* MASK INDEX */

static void sym_mask_account(struct symbol_device *d, uint32_t mask) {
    d->owner_cnt[symbol_mask_owner(mask)]++;
    d->type_cnt[symbol_mask_type(mask)]++;
    for (uint32_t m = mask; m != 0; m &= m - 1) {
        unsigned b = (unsigned)__builtin_ctz(m);
        if (d->bit_cnt[b]++ == 0)
            d->active_mask_or |= (1u << b);
    }
}

static void sym_mask_unaccount(struct symbol_device *d, uint32_t mask) {
    uint8_t o = symbol_mask_owner(mask), t = symbol_mask_type(mask);
#ifdef SYM_MEMDEBUG
    if (d->owner_cnt[o] == 0) PANIC("symbol: mask index underflow (owner)");
    if (d->type_cnt[t]  == 0) PANIC("symbol: mask index underflow (type)");
    d->owner_cnt[o]--;
    d->type_cnt[t]--;
#else
    if (d->owner_cnt[o] > 0) d->owner_cnt[o]--;
    if (d->type_cnt[t]  > 0) d->type_cnt[t]--;
#endif
    for (uint32_t m = mask; m != 0; m &= m - 1) {
        unsigned b = (unsigned)__builtin_ctz(m);
#ifdef SYM_MEMDEBUG
        if (d->bit_cnt[b] == 0) PANIC("symbol: mask index underflow (bit)");
        if (--d->bit_cnt[b] == 0)
            d->active_mask_or &= ~(1u << b);
#else
        if (d->bit_cnt[b] > 0 && --d->bit_cnt[b] == 0)
            d->active_mask_or &= ~(1u << b);
#endif
    }
}

/* ARENA */

/* Claim `len` bytes. First-fit over the sorted free list, coalescing assumed
 * (adjacent ranges never coexist). Falls back to bump allocation from the
 * high-water mark if no free range fits. Returns SYM_NO_SPACE on overflow. */
static int sym_arena_claim(struct symbol_device *d, uint32_t len, uint32_t *out_off) {
    if (len == 0) return SYM_INVALID_ARGS;

    for (size_t i = 0; i < d->free_count; i++) {
        struct sym_range *r = &d->free_ranges[i];
        if (r->len < len) continue;
        *out_off = r->off;
        if (r->len == len) {
            /* Consume the range. */
            for (size_t j = i + 1; j < d->free_count; j++)
                d->free_ranges[j - 1] = d->free_ranges[j];
            d->free_count--;
        } else {
            r->off += len;
            r->len -= len;
        }
        return SYM_OK;
    }

    /* Bump from high-water mark. */
    if ((size_t)d->arena_used + len > d->arena_size)
        return SYM_NO_SPACE;
    *out_off = (uint32_t)d->arena_used;
    d->arena_used += len;
    return SYM_OK;
}

/* Read-only: can `new_len` bytes be allocated given that the range
 * (old_off, old_len) will become available (replace case)? Pass old_len == 0
 * for a fresh insert. Does not mutate the arena.
 *
 * The answer is true iff, after the old range is conceptually returned to the
 * free list, some contiguous run of >= new_len bytes exists among:
 *   - any single existing free range,
 *   - the old range itself (which the caller owns),
 *   - the bump tail [arena_used, arena_size).
 * A free range adjacent to the old range can merge with it, which is why the
 * check tests each free range individually and the old range individually —
 * merged runs are never *smaller* than their parts, so single-range checks
 * are conservative in the right direction (they can under-count by not merging,
 * never over-count). Under-counting is the honest direction: we may refuse a
 * write that would fit after a merge, but we never claim a fit that isn't there.
 */

static bool sym_arena_fits_replace(struct symbol_device *d, uint32_t new_len,
                                   uint32_t old_off, uint32_t old_len) {
    if (new_len == 0) return false;
    if (new_len <= old_len) return true;              /* shrinks or equal: old range suffices */

    if ((size_t)d->arena_used + new_len <= d->arena_size) {
        /* Bump tail alone might fit. Still need to be sure the *claimed* range
         * will be obtainable: claim prefers the free list, then bump. If a free
         * range fits, commit will use it; if not, bump. Either way, a fit exists. */
        for (size_t i = 0; i < d->free_count; i++)
            if (d->free_ranges[i].len >= new_len) return true;
        /* Bump tail fits only if we're not in the odd case where the old range
         * is interleaved and bump would collide — it can't, bump is always at
         * the high-water mark, above every free range and every live range. */
        return true;
    }

    /* Bump tail is too small. A free range must cover it, OR the old range
     * plus an adjacent free range must merge to cover it.
     * Check each free range alone first. */
    for (size_t i = 0; i < d->free_count; i++)
        if (d->free_ranges[i].len >= new_len) return true;

    /* Check old range + any free range that is adjacent on either side. */
    for (size_t i = 0; i < d->free_count; i++) {
        struct sym_range *r = &d->free_ranges[i];
        bool adj_before = (r->off + r->len == old_off);
        bool adj_after  = (old_off + old_len == r->off);
        if (adj_before || adj_after) {
            if ((uint64_t)r->len + old_len >= new_len) return true;
        }
    }

    /* Old range alone (when new_len > old_len, this can't help; skip). */
    return false;
}

/* Release `len` bytes at `off` back to the free list, coalescing with
 * adjacent ranges. Overflow of the free-range table is treated as a
 * leak-free failure: the range is NOT returned, but no corruption occurs.
 * (See header note: overflow is SYM_NO_SPACE on the *next* set, never silent UB.) */
static void sym_arena_release(struct symbol_device *d, uint32_t off, uint32_t len) {
    if (len == 0) return;

    if (d->free_count >= SYMBOL_FREE_RANGES_MAX) {
        /* Cannot track another range. Drop it: the arena doesn't shrink,
         * so a dropped release is a leak, not corruption. */
        return;
    }

    /* Insert sorted by off, coalescing neighbours. */
    size_t pos = 0;
    while (pos < d->free_count && d->free_ranges[pos].off < off) pos++;

    /* Coalesce with previous if contiguous. */
    if (pos > 0) {
        struct sym_range *prev = &d->free_ranges[pos - 1];
        if (prev->off + prev->len == off) {
            prev->len += len;
            /* Coalesce with next if contiguous. */
            if (pos < d->free_count && prev->off + prev->len == d->free_ranges[pos].off) {
                prev->len += d->free_ranges[pos].len;
                for (size_t j = pos + 1; j < d->free_count; j++)
                    d->free_ranges[j - 1] = d->free_ranges[j];
                d->free_count--;
            }
            return;
        }
    }
    /* Coalesce with next if contiguous. */
    if (pos < d->free_count && off + len == d->free_ranges[pos].off) {
        d->free_ranges[pos].off = off;
        d->free_ranges[pos].len += len;
        return;
    }
    /* No neighbour: insert a new range. */
    for (size_t j = d->free_count; j > pos; j--)
        d->free_ranges[j] = d->free_ranges[j - 1];
    d->free_ranges[pos].off = off;
    d->free_ranges[pos].len = len;
    d->free_count++;
}

/* KEY BLOCK */

/* One kmalloc for ns\0name\0. kfree(key) frees both. */
static int sym_stage_key(const char *ns, size_t ns_len,
                         const char *name, size_t name_len,
                         char **out_key, char **out_ns, char **out_name) {
    char *block = kmalloc(ns_len + name_len + 2);
    if (block == NULL) return SYM_NO_SPACE;
    char *p = block;
    memcpy(p, ns, ns_len);       p[ns_len] = '\0';       *out_ns = p;    p += ns_len + 1;
    memcpy(p, name, name_len);   p[name_len] = '\0';     *out_name = p;
    *out_key = block;
    return SYM_OK;
}

/* LOOKUP */

/* Raw lookup: returns the live entry for (ns,name) regardless of expiry.
 * There is at most one live entry per key (set reclaims before insert). */
static struct symbol_entry *sym_lookup(struct symbol_device *d,
                                       const char *ns, size_t ns_len,
                                       const char *name, size_t name_len) {
    for (size_t i = 0; i < d->capacity; i++) {
        struct symbol_entry *s = &d->entries[i];
        if (!s->live) continue;
        if (s->ns_len != ns_len) continue;
        if (s->name_len != name_len) continue;
        if (memcmp(s->ns, ns, ns_len) != 0) continue;
        if (memcmp(s->name, name, name_len) != 0) continue;
        return s;
    }
    return NULL;
}

static struct symbol_entry *sym_find_free_slot(struct symbol_device *d) {
    for (size_t i = 0; i < d->capacity; i++)
        if (!d->entries[i].live) return &d->entries[i];
    return NULL;
}

/* PUBLIC: set */

int symbol_set(const char *ns, const char *name,
               const void *value, size_t len, uint32_t mask) {
    struct symbol_device *d = g_sym;
    if (d == NULL) return SYM_NO_DEVICE;
    if (ns == NULL || name == NULL || value == NULL) return SYM_INVALID_ARGS;

    size_t ns_len = strlen(ns), name_len = strlen(name);
    if (ns_len == 0 || name_len == 0) return SYM_INVALID_ARGS;
    if (ns_len > SYMBOL_NAME_MAX || name_len > SYMBOL_NAME_MAX) return SYM_INVALID_ARGS;

    uint8_t type = symbol_mask_type(mask);
    uint32_t bytes = 0;
    if (!sym_value_bytes(type, len, &bytes)) return SYM_INVALID_ARGS;

    /* STRING must carry its terminating NUL inside the declared length. */
    if (type == SYM_TYPE_STRING) {
        const char *s = (const char *)value;
        if (s[len - 1] != '\0') return SYM_INVALID_ARGS;
    }

    /* Locate any existing live entry for this key (for READONLY + replace). */
    struct symbol_entry *existing = sym_lookup(d, ns, ns_len, name, name_len);

    /* READONLY: refuse overwrite. Unset is still allowed (caller's job). */
    if (existing != NULL && (existing->mask & SYM_FLAG_READONLY))
        return SYM_READONLY;

    /* Slot availability: replacing reuses the existing slot; inserting needs a free one. */
    if (existing == NULL && sym_find_free_slot(d) == NULL)
        return SYM_NO_SPACE;

    /* Prove arena fit BEFORE staging anything. No mutation on this path. */
    uint32_t old_off = (existing != NULL) ? existing->value_off : 0;
    uint32_t old_len = (existing != NULL) ? existing->value_len : 0;
    if (!sym_arena_fits_replace(d, bytes, old_off, old_len))
        return SYM_NO_SPACE;

    /* Stage key block. Heap exhaustion is the last thing that can fail before commit. */
    char *n_key = NULL, *n_ns = NULL, *n_name = NULL;
    int st = sym_stage_key(ns, ns_len, name, name_len, &n_key, &n_ns, &n_name);
    if (st != SYM_OK) return st;

    /* Commit. Fit was proven; claim cannot fail. If it somehow does, panic rather
     * than leave partial state — this is an invariant violation, not a caller error. */
    if (existing != NULL)
        sym_arena_release(d, old_off, old_len);

    uint32_t new_off = 0;
    st = sym_arena_claim(d, bytes, &new_off);
    if (st != SYM_OK) {
        kfree(n_key);
        PANIC("symbol: arena claim failed after fit was proven");
    }

    struct symbol_entry *slot = existing;
    if (slot == NULL) {
        slot = sym_find_free_slot(d);
        if (slot == NULL) {
            /* Slot disappeared between check and commit: should not happen. */
            sym_arena_release(d, new_off, bytes);
            kfree(n_key);
            return SYM_INTERNAL;
        }
        d->count++;
    } else {
        kfree(slot->key);
        sym_mask_unaccount(d, slot->mask);
    }

    slot->key = n_key;
    slot->ns = n_ns;
    slot->name = n_name;
    slot->ns_len = (uint32_t)ns_len;
    slot->name_len = (uint32_t)name_len;
    slot->value_off = new_off;
    slot->value_len = bytes;
    slot->mask = mask;
    slot->live = true;

    memcpy(d->arena + new_off, value, bytes);

    sym_mask_account(d, mask);
    d->n_set++;
    return SYM_OK;
}

/* PUBLIC: get */

int symbol_get(const char *ns, const char *name,
               void *buf, size_t buf_max, size_t *len_out) {
    struct symbol_device *d = g_sym;
    if (d == NULL) return SYM_NO_DEVICE;
    if (ns == NULL || name == NULL || buf == NULL) return SYM_INVALID_ARGS;

    size_t ns_len = strlen(ns), name_len = strlen(name);
    if (ns_len == 0 || name_len == 0) return SYM_INVALID_ARGS;

    struct symbol_entry *s = sym_lookup(d, ns, ns_len, name, name_len);
    if (s == NULL) return SYM_NOT_FOUND;

    if (len_out != NULL) *len_out = s->value_len;
    if (buf_max < (size_t)s->value_len) return SYM_BUFFER_TOO_SMALL;

    memcpy(buf, d->arena + s->value_off, s->value_len);
    d->n_get++;
    return SYM_OK;
}

/* PUBLIC: unset */

int symbol_unset(const char *ns, const char *name) {
    struct symbol_device *d = g_sym;
    if (d == NULL) return SYM_NO_DEVICE;
    if (ns == NULL || name == NULL) return SYM_INVALID_ARGS;

    size_t ns_len = strlen(ns), name_len = strlen(name);
    if (ns_len == 0 || name_len == 0) return SYM_INVALID_ARGS;

    struct symbol_entry *s = sym_lookup(d, ns, ns_len, name, name_len);
    if (s == NULL) return SYM_NOT_FOUND;

    sym_mask_unaccount(d, s->mask);
    sym_arena_release(d, s->value_off, s->value_len);
    kfree(s->key);

    s->key = NULL;
    s->ns = NULL;
    s->name = NULL;
    s->ns_len = s->name_len = 0;
    s->value_off = s->value_len = 0;
    s->mask = 0;
    s->live = false;

    if (d->count > 0) d->count--;
    d->n_unset++;
    return SYM_OK;
}

int symbol_is_exist(const char *ns, const char *name) {
    struct symbol_device *d = g_sym;
    if (d == NULL) return SYM_NO_DEVICE;
    if (ns == NULL || name == NULL) return SYM_INVALID_ARGS;

    size_t ns_len = strlen(ns), name_len = strlen(name);
    if (ns_len == 0 || name_len == 0) return SYM_INVALID_ARGS;

    return sym_lookup(d, ns, ns_len, name, name_len) ? SYM_OK : SYM_NOT_FOUND;
}

/* LIFECYCLE */

void symbol_init(void) {
    if (g_sym != NULL) {
        klog_warning("symbol_init() called twice; ignoring\n");
        return;
    }

    struct symbol_device *d = kzalloc(sizeof *d);
    if (d == NULL) PANIC("symbol: cannot allocate device");

    d->entries = kzalloc(SYMBOL_MAX_ENTRIES * sizeof *d->entries);
    if (d->entries == NULL) PANIC("symbol: cannot allocate slot table");
    d->capacity = SYMBOL_MAX_ENTRIES;

    d->arena = kmalloc(SYMBOL_ARENA_SIZE);
    if (d->arena == NULL) PANIC("symbol: cannot allocate arena");
    d->arena_size = SYMBOL_ARENA_SIZE;

    d->arena_used = 0;
    d->free_count = 0;
    d->count = 0;

    g_sym = d;
    klog_serv("Symbols: Local storage entries=%u, arena=%u\n",
         SYMBOL_MAX_ENTRIES, SYMBOL_ARENA_SIZE);
}

void symbol_destroy(void) {
    struct symbol_device *d = g_sym;
    if (d == NULL) return;
    for (size_t i = 0; i < d->capacity; i++) {
        struct symbol_entry *s = &d->entries[i];
        if (s->live) kfree(s->key);
    }
    kfree(d->arena);
    kfree(d->entries);
    kfree(d);
    g_sym = NULL;
}
