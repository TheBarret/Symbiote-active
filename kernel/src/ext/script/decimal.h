#ifndef EXT_DEC_H
#define EXT_DEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <core/dev/env.h>

/* Mask private descriptor layout for decimal metadata */
#define DEC_P_SCALE(priv) (((priv) >> 8) & 0xFF)
#define DEC_P_ROUND(priv) (((priv) >> 4) & 0x0F)
#define DEC_P_FLAGS(priv) (((priv)) & 0x0F)
#define DEC_P_MAKE(s,r,f) ((((uint16_t)(s) & 0xFF) << 8) | \
                           (((uint16_t)(r) & 0x0F) << 4) | \
                            ((uint16_t)(f) & 0x0F))

#define DEC_F_SIGNED    0x1
#define DEC_F_SATURATE  0x2

/* Standard default scale and bounds */
#define DEC_DEFAULT_SCALE  4
#define DEC_MAX_SCALE      18

enum dec_round {
    DEC_RND_TRUNC     = 0,
    DEC_RND_HALF_UP   = 1,
    DEC_RND_HALF_EVEN = 2,
};

/* Signature parameter controlling default precision and behavior */
struct dec_signature {
    uint8_t default_scale;
    uint8_t default_round;
    uint8_t default_flags;
    bool    strict_overflow;
};

/* Decoded decimal type for VM operations */
struct dec {
    int64_t  mag;     /* Signed integer payload */
    uint8_t  scale;   /* Fractional digits */
    uint8_t  flags;   /* DEC_F_* */
    uint8_t  round;   /* DEC_RND_* */
    bool     ok;      /* False if parse/arithmetic failed */
};

/*  Service Interface  */

/**
 * @brief Fetch a decimal value from the VM environment under a namespace.
 */
struct dec dec_get(const struct env_ops *e, const char *ns, const char *name);

/**
 * @brief Store a decimal value into the VM environment with signature defaults.
 */
bool dec_set(const struct env_ops *e, const char *ns, const char *name,
             int64_t mag, int8_t scale, const struct dec_signature *sig, uint8_t importance);

/*  Internal VM Arithmetic Operations  */
bool dec_align(struct dec *a, struct dec *b, uint8_t *common);
struct dec dec_add(struct dec a, struct dec b, enum dec_round rnd, uint8_t result_scale);
struct dec dec_sub(struct dec a, struct dec b, enum dec_round rnd, uint8_t result_scale);
struct dec dec_mul(struct dec a, struct dec b, enum dec_round rnd, uint8_t result_scale);
struct dec dec_div(struct dec a, struct dec b, enum dec_round rnd, uint8_t result_scale);
struct dec dec_mod(struct dec a, struct dec b, enum dec_round rnd, uint8_t result_scale);

/* tester */
void test_decimal(void);

#endif /* EXT_DEC_H */
