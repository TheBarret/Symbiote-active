#ifndef CORE_LIB_SYMBOL_H
#define CORE_LIB_SYMBOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* codebase: 0.3.0 (flat names) */

/* Symbol store: bounded, fixed-capacity table of name -> typed value.
 *
 * Names are flat. Structure, if wanted, is a naming convention ("net.ip"),
 * not something the store knows about.
 */

/*  Mask layout
 *
 *  31      24 23    20 19    16 15                 0
 * +----------+--------+--------+-------------------+
 * |  OWNER   |  TYPE  | FLAGS  |     PRIVATE       |
 * | (8 bits) | (4 bit)| (4 bit)|    (16 bits)      |
 * +----------+--------+--------+-------------------+
 *
 * OWNER   opaque classification, decided higher up; the store does not interpret it.
 * TYPE    the decode key: how to read the value bytes. The load-bearing field.
 * FLAGS   policy bits the store enforces (see below).
 * PRIVATE subtype identity for a TYPE (e.g. decimal-as-INT metadata). Opaque to the store.
 */

#define SYMBOL_OWNER_SHIFT 24
#define SYMBOL_OWNER_MASK  (0xFFu << SYMBOL_OWNER_SHIFT)

#define SYMBOL_TYPE_SHIFT  20
#define SYMBOL_TYPE_MASK   (0xFu << SYMBOL_TYPE_SHIFT)

#define SYMBOL_FLAG_SHIFT  16
#define SYMBOL_FLAG_MASK   (0xFu << SYMBOL_FLAG_SHIFT)

#define SYMBOL_PRIV_SHIFT  0
#define SYMBOL_PRIV_MASK   (0xFFFFu << SYMBOL_PRIV_SHIFT)

/* TYPE values. The declared size is the arena slot size for that TYPE.
 * STRING is the only variable-width TYPE; it is bounded by SYMBOL_STRING_MAX. */
enum symbol_type {
    SYM_TYPE_NONE   = 0,   /* undefined / unset */
    SYM_TYPE_BYTE   = 1,   /* uint8_t,  1 byte  */
    SYM_TYPE_CHAR   = 2,   /* uint16_t, 2 bytes (code unit) */
    SYM_TYPE_FLOAT  = 3,   /* 4 bytes, software float; PRIVATE carries format/scale */
    SYM_TYPE_INT16  = 4,   /* int16_t,  2 bytes */
    SYM_TYPE_INT32  = 5,   /* int32_t,  4 bytes */
    SYM_TYPE_UINT16 = 6,   /* uint16_t, 2 bytes */
    SYM_TYPE_UINT32 = 7,   /* uint32_t, 4 bytes */
    SYM_TYPE_BOOL   = 8,   /* 1 byte, 0 or 1 */
    SYM_TYPE_STRING = 9,   /* bounded byte string, NUL-terminated, <= SYMBOL_STRING_MAX */
    /* 10..15 reserved */
};

/* FLAGS: policy bits. Orthogonal and combinable. */
#define SYM_FLAG_READONLY  (1u << 16)  /* write-once; after first set, unset allowed, overwrite refused */
#define SYM_FLAG_SKIP      (1u << 17)  /* always skipped in enumeration; reads by name still work */
#define SYM_FLAG_CACHEABLE (1u << 18)  /* volatile, safe to recompute. */
#define SYM_FLAG_SYSTEM    (1u << 19)  /* system data, sensitive; policy decided above the store */

/* Mask Builder
 * Extensions MUST build masks with symbol_mask_build */
static inline uint32_t symbol_mask_build(uint8_t owner, uint8_t type, uint32_t flags, uint16_t priv) {
    return ((uint32_t)(owner & 0xFFu) << SYMBOL_OWNER_SHIFT) |
           ((uint32_t)(type  & 0x0Fu) << SYMBOL_TYPE_SHIFT)  |
           (flags & SYMBOL_FLAG_MASK) |
           ((uint32_t)priv << SYMBOL_PRIV_SHIFT);
}
static inline uint8_t  symbol_mask_owner(uint32_t m) { return (uint8_t)((m & SYMBOL_OWNER_MASK) >> SYMBOL_OWNER_SHIFT); }
static inline uint8_t  symbol_mask_type (uint32_t m) { return (uint8_t)((m & SYMBOL_TYPE_MASK)  >> SYMBOL_TYPE_SHIFT);  }
/* shifted to 0..15 */
static inline uint8_t  symbol_mask_flags(uint32_t m) { return (uint8_t)((m & SYMBOL_FLAG_MASK)  >> SYMBOL_FLAG_SHIFT);  }
static inline uint16_t symbol_mask_priv (uint32_t m) { return (uint16_t)((m & SYMBOL_PRIV_MASK)  >> SYMBOL_PRIV_SHIFT);  }
static inline bool symbol_mask_has(uint32_t m, uint32_t flag) { return (m & flag) == flag; }


/* Return codes */
enum symbol_result {
    SYM_OK               =  0,
    SYM_NOT_FOUND        = -1,
    SYM_READONLY         = -2,
    SYM_NO_SPACE         = -4,   /* arena or slot table exhausted */
    SYM_INVALID_ARGS     = -5,
    SYM_NO_DEVICE        = -6,
    SYM_INTERNAL         = -7,
    SYM_BUFFER_TOO_SMALL = -8,   /* <*len_out> holds needed length */
};


/*  Arena
 *
 * One fixed byte arena, allocated once at init, never grows.
 * Entries reference arena ranges by offset (not raw pointer),
 * so compaction is permitted later without chasing holders.
 *
 * Slot size by TYPE:
 *   BYTE/BOOL          1
 *   CHAR/INT16/UINT16  2
 *   INT32/UINT32/FLOAT 4
 *   STRING             variable (bounded by SYMBOL_STRING_MAX, includes NUL)
 *
 * Allocation: first-fit over a sorted, coalescing free-range list, with
 * bump allocation from the high-water mark as the fallback. Releasing the
 * topmost range lowers the high-water mark again. Exhaustion fails the write:
 * all-or-nothing, nothing evicted on failure.
 */

/* Memory footprint */
#define SYMBOL_STRING_MAX      256u           /* max STRING payload incl. NUL */
#define SYMBOL_ARENA_SIZE      (64u * 1024u)  /* total arena bytes */
#define SYMBOL_MAX_ENTRIES     256u           /* slot table capacity */
#define SYMBOL_NAME_MAX        64u            /* max name length (bytes, excl. NUL) */
#define SYMBOL_FREE_RANGES_MAX 64u            /* max tracked free ranges */

/*  Entry
 *
 * The name is stored inline, so an entry costs no heap allocation.
 * The value lives in the arena, referenced by offset.
 */
struct symbol_entry {
    char     name[SYMBOL_NAME_MAX + 1];
    uint32_t name_len;

    uint32_t value_off;  /* offset into arena */
    uint32_t value_len;  /* payload bytes as stored; for STRING, includes the NUL */

    uint32_t mask;       /* OWNER | TYPE | FLAGS | PRIVATE */
    bool     live;
};

/*  Public API
 *
 * `name` must be a NUL-terminated string of 1..SYMBOL_NAME_MAX bytes.
 */

void symbol_init(void);
void symbol_destroy(void);

/* Write a typed value. TYPE in `mask` selects how `value`/`len` is read.
 *
 * (type, len) must be consistent:
 *   - fixed-width TYPE: len == the TYPE's declared width
 *   - SYM_TYPE_STRING:  0 < len <= SYMBOL_STRING_MAX, and value[len-1] == '\0'
 *   - any other combination: SYM_INVALID_ARGS
 *
 * All-or-nothing: failure leaves the store exactly as it was.
 * WRITE-ONCE for SYM_FLAG_READONLY entries: first set may proceed,
 * later overwrite returns SYM_READONLY (unset still allowed).
 */
int symbol_set(const char *name, const void *value, size_t len, uint32_t mask);

/* Read a typed value into a caller buffer.
 * Copies exactly value_len bytes; no implicit NUL is appended.
 * A STRING value includes its own NUL (set required it), so it is self-terminating.
 * On SYM_BUFFER_TOO_SMALL, *len_out holds the needed length.
 */
int symbol_get(const char *name, void *buf, size_t buf_max, size_t *len_out);

/* (Nuclear) Remove: returns its arena range to the pool.
 * Not gated by flags (READONLY/SYSTEM), no caller tracing.
 */
int symbol_unset(const char *name);

/* Existence. */
int symbol_is_exist(const char *name);

#endif /* CORE_LIB_SYMBOL_H */
