#ifndef CORE_ENV_MASK_H
#define CORE_ENV_MASK_H

#include <stdbool.h>
#include <stdint.h>

/* codebase: 0.1.2 */

/* Mask Contracts
 *
 * The environment store treats this 32-bit field as entirely opaque.
 * This header defines the schema that extensions and the core agree upon.
 *
 * Bit layout:
 *
 *  31      24 23    20 19    16 15                 0
 * +----------+--------+--------+-------------------+
 * |  OWNER   |  TYPE  | FLAGS  |     PRIVATE       |
 * | (8 bits) | (4 bit)| (4 bit)|    (16 bits)      |
 * +----------+--------+--------+-------------------+
 *
 * Rules of engagement:
 *   1. Extensions MUST construct masks using env_mask_build().
 *      Do not bit-shift manually; future layout changes will break you.
 *   2. Extensions may only set bits in the PRIVATE field directly.
 *   3. The OWNER field is populated with the extension's bus ID.
 *   4. The TYPE field is a hint for UI and tooling, not a binding contract.
 *      The value string remains a string; TYPE just describes its intent.
 *   5. FLAGS are universal, core-defined behaviors.
 *
 * Querying (IMPORTANT):
 *   OWNER and TYPE are NUMBERS, not bitsets. A subset test on them is wrong:
 *   owner 1 is a subset of owners 3, 5 and 7. Match them with a care/want pair,
 *   (mask & care) == want, using the ENV_Q_* helpers below with
 *   env_ops->count_match() / list_match(). Subset matching (the legacy
 *   *_by_mask calls) is only meaningful for FLAGS and PRIVATE bits.
 */

/* OWNER (bits 31..24) */
#define ENV_MASK_OWNER_SHIFT   24
#define ENV_MASK_OWNER_MASK    (0xFFu << ENV_MASK_OWNER_SHIFT)

/* TYPE (bits 23..20) */
#define ENV_MASK_TYPE_SHIFT    20
#define ENV_MASK_TYPE_MASK     (0xFu << ENV_MASK_TYPE_SHIFT)

#define ENV_TYPE_NONE          0u   /* opaque blob, unset */
#define ENV_TYPE_STRING        1u   /* printable ANSI/ASCII/UTF-8 */
#define ENV_TYPE_INT           2u   /* decimal integer*/
#define ENV_TYPE_BOOL          3u   /* "0" or "1" */
#define ENV_TYPE_HEX           4u   /* "0xDEADBEEF" */
#define ENV_TYPE_PATH          5u   /* filesystem path*/
#define ENV_TYPE_ENUM          6u   /* one of a defined set */
#define ENV_TYPE_STRUCT        7u   /* structured */
#define ENV_TYPE_BIN           8u   /* hex-encoded raw bytes */

/* FLAGS (bits 19..16) */
#define ENV_MASK_FLAG_SHIFT    16
#define ENV_MASK_FLAG_MASK     (0xFu << ENV_MASK_FLAG_SHIFT)

#define ENV_FLAG_READONLY      (1u << 16)  /* policy: one-shot, no overwrites */
#define ENV_FLAG_SECRET        (1u << 17)  /* policy: hide from default listings */
#define ENV_FLAG_CACHEABLE     (1u << 18)  /* policy: safe to recompute (evicted first) */
#define ENV_FLAG_PINNED        (1u << 19)  /* policy: immune to space-pressure eviction */

/* PRIVATE (bits 15..0) */
#define ENV_MASK_PRIVATE_SHIFT 0
#define ENV_MASK_PRIVATE_MASK  (0xFFFFu << ENV_MASK_PRIVATE_SHIFT)

/* Mask Builder */

static inline uint32_t env_mask_build(uint16_t owner_id, uint8_t type,
                                      uint32_t flags_word, uint16_t priv) {
    return ((uint32_t)(owner_id & 0xFFu) << ENV_MASK_OWNER_SHIFT) |
           ((uint32_t)(type & 0xFu) << ENV_MASK_TYPE_SHIFT) |
           (flags_word & ENV_MASK_FLAG_MASK) |
           ((uint32_t)(priv) << ENV_MASK_PRIVATE_SHIFT);
}

/* Extraction Helpers (for envtool, decoders, and UI) */
static inline uint8_t  env_mask_owner(uint32_t mask) {
    return (uint8_t)((mask & ENV_MASK_OWNER_MASK) >> ENV_MASK_OWNER_SHIFT);
}
static inline uint8_t  env_mask_type(uint32_t mask) {
    return (uint8_t)((mask & ENV_MASK_TYPE_MASK) >> ENV_MASK_TYPE_SHIFT);
}
/* Returns the flags SHIFTED DOWN to 0..15.
 * Do NOT test the result against ENV_FLAG_* (those are unshifted, 1u << 16..19);
 * use env_mask_has() instead. */
static inline uint8_t  env_mask_flags(uint32_t mask) {
    return (uint8_t)((mask & ENV_MASK_FLAG_MASK) >> ENV_MASK_FLAG_SHIFT);
}
static inline uint16_t env_mask_priv(uint32_t mask) {
    return (uint16_t)((mask & ENV_MASK_PRIVATE_MASK) >> ENV_MASK_PRIVATE_SHIFT);
}

/* True if every bit of `flag` (an ENV_FLAG_* constant, unshifted) is set. */
static inline bool env_mask_has(uint32_t mask, uint32_t flag) {
    return (mask & flag) == flag;
}

/* Query helpers. Each expands to TWO arguments, (care, want), so:
 *   env->count_match(ENV_Q_OWNER(BUS_VM16), &n);
 *   env->count_match(ENV_Q_FLAG(ENV_FLAG_PINNED), &n);
 * Combine several terms by OR-ing the care halves and the want halves yourself.
 * OR-ing two ENV_Q_FLAG terms requires both flags (care = f1|f2, want = f1|f2); it is not "either." */
#define ENV_Q_OWNER(id)  ENV_MASK_OWNER_MASK, (((uint32_t)(id) & 0xFFu) << ENV_MASK_OWNER_SHIFT)
#define ENV_Q_TYPE(t)    ENV_MASK_TYPE_MASK,  (((uint32_t)(t) & 0xFu) << ENV_MASK_TYPE_SHIFT)
#define ENV_Q_FLAG(f)    (f), (f)

/* Type to String (for diagnostic tools) */
static inline const char *env_type_str(uint8_t type) {
    switch (type) {
        case ENV_TYPE_NONE:   return "none";
        case ENV_TYPE_STRING: return "string";
        case ENV_TYPE_INT:    return "int";
        case ENV_TYPE_BOOL:   return "bool";
        case ENV_TYPE_HEX:    return "hex";
        case ENV_TYPE_PATH:   return "path";
        case ENV_TYPE_ENUM:   return "enum";
        case ENV_TYPE_STRUCT: return "struct";
        case ENV_TYPE_BIN:    return "bin";
        default:              return "undefined";
    }
}

#endif /* CORE_ENV_MASK_H */
