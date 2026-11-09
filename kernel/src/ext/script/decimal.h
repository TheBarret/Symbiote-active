#ifndef EXT_DEC_H
#define EXT_DEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <core/dev/env.h>

/* Software decimals: signed 64-bit magnitude + out-of-band scale (digits after the point).
 * No FPU/SSE. All intermediates are 128-bit and every overflow is checked,
 * so an operation either returns the exactly-rounded result or ok == false, never wraps or traps.
 *
 * Contract (all arithmetic):
 *   - inputs must have ok == true and scale <= DEC_MAX_SCALE
 *   - result_scale must be <= DEC_MAX_SCALE
 *   - the result ALWAYS has scale == result_scale (up or down)
 *   - rounding only happens when digits are discarded
 *   - failure is reported as ok == false: bad input, divide by zero, or a result that does not fit int64
 *   - DEC_RND_HALF_UP rounds ties AWAY from zero (-2.5 -> -3)
 *   - dec_mod: remainder takes the sign of the dividend (C semantics)
 *   - flags must be 0 or DEC_F_SIGNED: DEC_F_SATURATE is not implemented and unknown bits are rejected by dec_set and dec_get alike
 */

/* Mask private descriptor layout for decimal metadata */
#define DEC_P_SCALE(priv) (((priv) >> 8) & 0xFF)
#define DEC_P_ROUND(priv) (((priv) >> 4) & 0x0F)
#define DEC_P_FLAGS(priv) (((priv)) & 0x0F)
#define DEC_P_MAKE(s,r,f) ((((uint16_t)(s) & 0xFF) << 8) | \
                           (((uint16_t)(r) & 0x0F) << 4) | \
                            ((uint16_t)(f) & 0x0F))

#define DEC_F_SIGNED    0x1
#define DEC_F_SATURATE  0x2   /* not implemented: rejected by dec_set/dec_get */

/* Standard default scale and bounds */
#define DEC_DEFAULT_SCALE  4
#define DEC_MAX_SCALE      18

enum dec_round {
    DEC_RND_TRUNC     = 0,   /* toward zero */
    DEC_RND_HALF_UP   = 1,   /* ties away from zero (NOT IEEE "toward +inf") */
    DEC_RND_HALF_EVEN = 2,   /* ties to even */
};

/* Signature parameter controlling default precision and behavior */
struct dec_signature {
    uint8_t default_scale;
    uint8_t default_round;
    uint8_t default_flags;
    bool    strict_overflow;   /* reserved: must be false; overflow is always strict today */
};

/* Decoded decimal type for VM operations */
struct dec {
    int64_t  mag;     /* Signed integer payload */
    uint8_t  scale;   /* Fractional digits */
    uint8_t  flags;   /* DEC_F_* (validated: 0 or DEC_F_SIGNED) */
    uint8_t  round;   /* DEC_RND_* */
    bool     ok;      /* False if parse/arithmetic failed */
};

/*  Service Interface  */

/**
 * @brief Fetch a decimal from env namespace "dec.<ns>".
 *
 * Fails (ok == false) unless the entry is ENV_TYPE_INT, owned by BUS_VM16 or BUS_PROXY,
 * carries a valid scale/rounding/flags in its mask, and holds a canonical base-10 integer that fits int64.
 */
struct dec dec_get(const struct env_ops *e, const char *ns, const char *name);

/**
 * @brief Store a decimal into env namespace "dec.<ns>".
 *
 * scale < 0 means "use sig->default_scale (or DEC_DEFAULT_SCALE)".
 * Fails on a bad scale/rounding, DEC_F_SATURATE or unknown flag bits,
 * an over-long namespace, or an env failure.
 *
 * Ownership note: entries are always stamped owner BUS_VM16 (the decimal service's identity),
 * even when reached through the proxy; dec_get accepts both BUS_VM16- and BUS_PROXY-owned entries.
 */
bool dec_set(const struct env_ops *e, const char *ns, const char *name,
             int64_t mag, int8_t scale, const struct dec_signature *sig, uint8_t importance);

/*  Internal VM Arithmetic Operations  */

/* Rescale both operands upward to the larger scale. The rescale is exact by construction
 * (digits are only added, never dropped), so there is no rounding.
 * Fails, without modifying either operand, if a rescaled magnitude does not fit int64.
 * Note: dec_add/dec_sub align internally in 128 bits and can therefore succeed where dec_align fails. */
bool dec_align(struct dec *a, struct dec *b, uint8_t *common);
struct dec dec_add(struct dec a, struct dec b, enum dec_round rnd, uint8_t result_scale);
struct dec dec_sub(struct dec a, struct dec b, enum dec_round rnd, uint8_t result_scale);
struct dec dec_mul(struct dec a, struct dec b, enum dec_round rnd, uint8_t result_scale);
struct dec dec_div(struct dec a, struct dec b, enum dec_round rnd, uint8_t result_scale);
struct dec dec_mod(struct dec a, struct dec b, enum dec_round rnd, uint8_t result_scale);

/** CMP operations
 * Exact comparison across scales: operands are compared at a common scale.
 * Returns:
 *   -1 if a < b
 *    0 if a == b
 *    1 if a > b
 *   -2 on error (invalid operand: !dec_valid, or an internal rescale failure)
 *
 * Caller contract (VM/trap layer): -2 is a FAULT, not a comparison result.
 * It must be distinguished from -1 and never mapped onto condition codes
 * (N/Z/P); a failed compare must not branch as "less than".
 * The int64 fast path is dec_align; when the rescale overflows int64 the
 * exact u128 fallback decides, so dec_cmp succeeds where dec_align fails.
 */
int dec_cmp(struct dec a, struct dec b);

#endif /* EXT_DEC_H */
