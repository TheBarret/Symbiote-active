#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <lib/mem.h>
#include <lib/string.h>
#include <lib/symbol.h>
#include <core/arch/heap.h>
#include <core/panic.h>
#include <core/klog.h>

/* codebase: 0.3.0 (flat names) */

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
    uint64_t             n_leaked_bytes;   /* releases that could not be tracked */
};

static struct symbol_device *g_sym;

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

/* NAME */

/* Bounded length scan: never reads past SYMBOL_NAME_MAX + 1 bytes, so an
 * unterminated name from a bus payload cannot run away. */
static bool sym_name_len(const char *name, size_t *out_len) {
    size_t n = 0;
    while (n <= SYMBOL_NAME_MAX && name[n] != '\0') n++;
    if (n == 0 || n > SYMBOL_NAME_MAX) return false;
    *out_len = n;
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

/* Claim `len` bytes. First-fit over the sorted free list (adjacent ranges
 * never coexist), falling back to bump allocation from the high-water mark.
 * Returns SYM_NO_SPACE on overflow. Mutates nothing on failure. */
static int sym_arena_claim(struct symbol_device *d, uint32_t len, uint32_t *out_off) {
    if (len == 0) return SYM_INVALID_ARGS;

    for (size_t i = 0; i < d->free_count; i++) {
        struct sym_range *r = &d->free_ranges[i];
        if (r->len < len) continue;
        *out_off = r->off;
        if (r->len == len) {
            for (size_t j = i + 1; j < d->free_count; j++)
                d->free_ranges[j - 1] = d->free_ranges[j];
            d->free_count--;
        } else {
            r->off += len;
            r->len -= len;
        }
        return SYM_OK;
    }

    if ((size_t)d->arena_used + len > d->arena_size)
        return SYM_NO_SPACE;
    *out_off = (uint32_t)d->arena_used;
    d->arena_used += len;
    return SYM_OK;
}

/* If the last free range touches the high-water mark, give it back to the
 * bump region. Keeps the free table short and lets the tail be reused. */
static void sym_arena_trim_tail(struct symbol_device *d) {
    if (d->free_count == 0) return;
    struct sym_range *r = &d->free_ranges[d->free_count - 1];
    if ((size_t)r->off + r->len == d->arena_used) {
        d->arena_used = r->off;
        d->free_count--;
    }
}

/* Release `len` bytes at `off`, coalescing with neighbours. Merging never
 * needs a new table slot, so it cannot fail. Only a range with no neighbour
 * needs a slot; if the table is full that range is leaked (counted in stats),
 * never corrupted. */
static void sym_arena_release(struct symbol_device *d, uint32_t off, uint32_t len) {
    if (len == 0) return;

    size_t pos = 0;
    while (pos < d->free_count && d->free_ranges[pos].off < off) pos++;

    bool merge_prev = pos > 0 &&
        d->free_ranges[pos - 1].off + d->free_ranges[pos - 1].len == off;
    bool merge_next = pos < d->free_count &&
        off + len == d->free_ranges[pos].off;

    if (merge_prev && merge_next) {
        d->free_ranges[pos - 1].len += len + d->free_ranges[pos].len;
        for (size_t j = pos + 1; j < d->free_count; j++)
            d->free_ranges[j - 1] = d->free_ranges[j];
        d->free_count--;
    } else if (merge_prev) {
        d->free_ranges[pos - 1].len += len;
    } else if (merge_next) {
        d->free_ranges[pos].off = off;
        d->free_ranges[pos].len += len;
    } else if ((size_t)off + len == d->arena_used) {
        /* topmost block, no neighbour: just lower the high-water mark */
        d->arena_used = off;
        return;
    } else {
        if (d->free_count >= SYMBOL_FREE_RANGES_MAX) {
            d->n_leaked_bytes += len;
            return;
        }
        for (size_t j = d->free_count; j > pos; j--)
            d->free_ranges[j] = d->free_ranges[j - 1];
        d->free_ranges[pos].off = off;
        d->free_ranges[pos].len = len;
        d->free_count++;
    }
    sym_arena_trim_tail(d);
}

/* LOOKUP */

/* Returns the live entry for `name`. There is at most one per name. */
static struct symbol_entry *sym_lookup(struct symbol_device *d,
                                       const char *name, size_t name_len) {
    for (size_t i = 0; i < d->capacity; i++) {
        struct symbol_entry *s = &d->entries[i];
        if (!s->live) continue;
        if (s->name_len != name_len) continue;
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

int symbol_set(const char *name, const void *value, size_t len, uint32_t mask) {
    struct symbol_device *d = g_sym;
    if (d == NULL) return SYM_NO_DEVICE;
    if (name == NULL || value == NULL) return SYM_INVALID_ARGS;

    size_t name_len;
    if (!sym_name_len(name, &name_len)) return SYM_INVALID_ARGS;

    uint8_t type = symbol_mask_type(mask);
    uint32_t bytes = 0;
    if (!sym_value_bytes(type, len, &bytes)) return SYM_INVALID_ARGS;

    /* STRING must carry its terminating NUL inside the declared length. */
    if (type == SYM_TYPE_STRING && ((const char *)value)[len - 1] != '\0')
        return SYM_INVALID_ARGS;

    struct symbol_entry *existing = sym_lookup(d, name, name_len);

    /* READONLY: refuse overwrite. Unset is still allowed (caller's job). */
    if (existing != NULL && (existing->mask & SYM_FLAG_READONLY))
        return SYM_READONLY;

    struct symbol_entry *slot = existing ? existing : sym_find_free_slot(d);
    if (slot == NULL) return SYM_NO_SPACE;

    /* Place the value. The only step that can fail, and it runs before any
     * mutation of the entry, so failure leaves the store exactly as it was.
     *  - shrink/equal replace: reuse the old offset, give the tail back
     *  - otherwise: claim a new range, then release the old one
     * (growing a value briefly needs old + new space; no panic path.) */
    uint32_t off;
    if (existing != NULL && bytes <= existing->value_len) {
        off = existing->value_off;
        sym_arena_release(d, off + bytes, existing->value_len - bytes);
    } else {
        int st = sym_arena_claim(d, bytes, &off);
        if (st != SYM_OK) return st;
        if (existing != NULL)
            sym_arena_release(d, existing->value_off, existing->value_len);
    }

    if (existing != NULL) {
        sym_mask_unaccount(d, existing->mask);
    } else {
        memcpy(slot->name, name, name_len);
        slot->name[name_len] = '\0';
        slot->name_len = (uint32_t)name_len;
        d->count++;
    }

    slot->value_off = off;
    slot->value_len = bytes;
    slot->mask      = mask;
    slot->live      = true;

    memcpy(d->arena + off, value, bytes);

    sym_mask_account(d, mask);
    d->n_set++;
    return SYM_OK;
}

/* PUBLIC: get */

int symbol_get(const char *name, void *buf, size_t buf_max, size_t *len_out) {
    struct symbol_device *d = g_sym;
    if (d == NULL) return SYM_NO_DEVICE;
    if (name == NULL || buf == NULL) return SYM_INVALID_ARGS;

    size_t name_len;
    if (!sym_name_len(name, &name_len)) return SYM_INVALID_ARGS;

    struct symbol_entry *s = sym_lookup(d, name, name_len);
    if (s == NULL) return SYM_NOT_FOUND;

    if (len_out != NULL) *len_out = s->value_len;
    if (buf_max < (size_t)s->value_len) return SYM_BUFFER_TOO_SMALL;

    memcpy(buf, d->arena + s->value_off, s->value_len);
    d->n_get++;
    return SYM_OK;
}

/* PUBLIC: unset */

int symbol_unset(const char *name) {
    struct symbol_device *d = g_sym;
    if (d == NULL) return SYM_NO_DEVICE;
    if (name == NULL) return SYM_INVALID_ARGS;

    size_t name_len;
    if (!sym_name_len(name, &name_len)) return SYM_INVALID_ARGS;

    struct symbol_entry *s = sym_lookup(d, name, name_len);
    if (s == NULL) return SYM_NOT_FOUND;

    sym_mask_unaccount(d, s->mask);
    sym_arena_release(d, s->value_off, s->value_len);

    s->name[0]   = '\0';
    s->name_len  = 0;
    s->value_off = s->value_len = 0;
    s->mask      = 0;
    s->live      = false;

    if (d->count > 0) d->count--;
    d->n_unset++;
    return SYM_OK;
}

int symbol_is_exist(const char *name) {
    struct symbol_device *d = g_sym;
    if (d == NULL) return SYM_NO_DEVICE;
    if (name == NULL) return SYM_INVALID_ARGS;

    size_t name_len;
    if (!sym_name_len(name, &name_len)) return SYM_INVALID_ARGS;

    return sym_lookup(d, name, name_len) ? SYM_OK : SYM_NOT_FOUND;
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

    g_sym = d;
    klog_serv("Symbols: entries=%u, arena=%u\n",
              SYMBOL_MAX_ENTRIES, SYMBOL_ARENA_SIZE);
}

void symbol_destroy(void) {
    struct symbol_device *d = g_sym;
    if (d == NULL) return;
    kfree(d->arena);
    kfree(d->entries);
    kfree(d);
    g_sym = NULL;
}
