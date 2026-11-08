#include <lib/string.h>
#include <core/dev/env.h>
#include <core/mask.h>
#include <core/bus.h>
#include <core/kprintf.h>
#include <ext/script/decimal.h>

/* Design
 * Every operation is done in sign + magnitude form on unsigned 128-bit integers,
 * then rounded once, at the end, by round_div().
 * Magnitudes (never signed values) are used so INT64_MIN needs no special case,
 * and no signed overflow or hardware trap (#DE) is reachable.
 *
 * 128-bit division is done in software (udivmod128) so the kernel does not need libgcc's __udivti3/__umodti3.
 * Shifts, compares, subtracts and 64x64->128 multiplies are inlined by the compiler.
 *
 * Invariant of dec_div: a scaling overflow reject is never spurious. The divisor
 * magnitude stays < 2^63, so if the scaled dividend no longer fits u128, the
 * quotient is >= 2^128 / 2^63 = 2^65 and cannot fit int64 anyway. Rejection
 * always means "unrepresentable result", never "representable but gave up".
 *
 * Environment symbols storage come with private headers:
 *
 * mask.h defines the 32-bit metadata field layout:
 *
 *       31       24 23    20 19    16 15                 0
 *       +----------+--------+--------+-------------------+
 *       |  OWNER   |  TYPE  | FLAGS  |     PRIVATE       |
 *       | (8 bits) |(4 bits)|(4 bits)|    (16 bits)      |
 *       +----------+--------+--------+--------+----------+
 *       | IDENTITY:ENV_TYPE_INT      | SCALE|ROUND|FLAGS |  [DECIMAL.H]
 *       +----------------------------+------+-----+------+
 *
 */
#if !defined(__SIZEOF_INT128__)
#error "decimal.c requires __int128 (GCC/Clang, x86-64)"
#endif

typedef unsigned __int128 u128;

/* floor(UINT128_MAX / 10), written out so no division is emitted */
#define U128_DIV10 ((((u128)0x1999999999999999ull) << 64) | (u128)0x9999999999999999ull)

/*  128-bit helpers  */

/* n / d and n % d. d must be non-zero and < 2^127 (callers stay below 2^124). */
static u128 udivmod128(u128 n, u128 d, u128 *rem)
{
    if (d == 1) {                                /* common: every upscale divides by 1 */
        *rem = 0;
        return n;
    }
    if ((n >> 64) == 0 && (d >> 64) == 0) {      /* fast path: native 64-bit div */
        uint64_t n64 = (uint64_t)n, d64 = (uint64_t)d;
        *rem = (u128)(n64 % d64);
        return (u128)(n64 / d64);
    }
    u128 q = 0, r = 0;
    int i = 127;
    while (i >= 0 && ((n >> i) & 1) == 0)        /* skip leading zeros: q's high bits are 0 */
        i--;
    for (; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1);
        if (r >= d) {
            r -= d;
            q |= (u128)1 << i;
        }
    }
    *rem = r;
    return q;
}

/* *v *= 10^e, false on overflow */
static bool u128_mul_pow10(u128 *v, unsigned e)
{
    for (; e; e--) {
        if (*v > U128_DIV10) return false;
        *v *= 10;
    }
    return true;
}

/* |v| as u128, sign in *neg. Correct for INT64_MIN. */
static inline u128 mag_of(int64_t v, bool *neg)
{
    *neg = v < 0;
    return v < 0 ? (u128)(0 - (uint64_t)v) : (u128)(uint64_t)v;
}

static inline bool rnd_valid(enum dec_round r) { return (unsigned)r <= (unsigned)DEC_RND_HALF_EVEN; }
static inline bool dec_valid(const struct dec *d) { return d->ok && d->scale <= DEC_MAX_SCALE; }

/* result = round(num / den), applying the sign, into int64.
 * den != 0 and den < 2^123. False if the rounded result does not fit. */
static bool round_div(u128 num, u128 den, bool neg, enum dec_round rnd, int64_t *out)
{
    if (!rnd_valid(rnd) || den == 0) return false;

    u128 rem;
    u128 q = udivmod128(num, den, &rem);

    if (rem != 0) {
        u128 twice = rem << 1;                   /* rem < den < 2^123: no overflow */
        bool up;
        switch (rnd) {
        case DEC_RND_TRUNC:     up = false; break;
        case DEC_RND_HALF_UP:   up = (twice >= den); break;
        case DEC_RND_HALF_EVEN: up = (twice > den) || (twice == den && (q & 1)); break;
        default:                return false;
        }
        if (up) q++;
    }

    if (neg) {
        if (q > ((u128)1 << 63)) return false;   /* INT64_MIN magnitude is 2^63 */
        *out = (int64_t)(0 - (uint64_t)q);
    } else {
        if (q > (u128)INT64_MAX) return false;
        *out = (int64_t)(uint64_t)q;
    }
    return true;
}

/* round( (neg ? -mag : mag) * 10^up / 10^down ) */
static bool scale_signed(bool neg, u128 mag, unsigned up, unsigned down,
                         enum dec_round rnd, int64_t *out)
{
    u128 den = 1;
    if (!u128_mul_pow10(&mag, up))   return false;
    if (!u128_mul_pow10(&den, down)) return false;
    return round_div(mag, den, neg, rnd, out);
}

/* Rescale an int64 payload upward (to >= from). Exact by construction:
 * digits are only ever appended, so there is no rounding to configure.
 * False if the rescaled magnitude does not fit int64. */
static bool upscale_i64(int64_t x, uint8_t from, uint8_t to, int64_t *out)
{
    if (from > DEC_MAX_SCALE || to > DEC_MAX_SCALE || to < from) return false;
    bool neg;
    u128 m = mag_of(x, &neg);
    return scale_signed(neg, m, (unsigned)(to - from), 0, DEC_RND_TRUNC, out);
}

/* |d| rescaled up to `common` (>= d->scale), exactly, as u128. */
static bool align_mag(const struct dec *d, uint8_t common, bool *neg, u128 *m)
{
    *m = mag_of(d->mag, neg);
    return u128_mul_pow10(m, (unsigned)(common - d->scale));
}

/* sign + magnitude addition; operands are < 2^124 so the sum cannot overflow */
static void signed_sum(bool sa, u128 ma, bool sb, u128 mb, bool *sr, u128 *mr)
{
    if (sa == sb)      { *mr = ma + mb; *sr = sa; }
    else if (ma >= mb) { *mr = ma - mb; *sr = sa; }
    else               { *mr = mb - ma; *sr = sb; }
    if (*mr == 0) *sr = false;
}

/*  env namespace / parsing helpers  */

/* "dec." + ns into buf. False (no silent truncation) if it does not fit. */
static bool build_ns(const char *ns, char *buf, size_t max_len)
{
    size_t n = 0;
    for (const char *p = "dec."; *p; p++) {
        if (n + 1 >= max_len) return false;
        buf[n++] = *p;
    }
    if (ns) {
        for (; *ns; ns++) {
            if (n + 1 >= max_len) return false;
            buf[n++] = *ns;
        }
    }
    buf[n] = '\0';
    return true;
}

/* Canonical base-10 int64: optional '-', 1+ digits, no leading zeros,
 * no "-0", nothing else. Rejects "+5", "007", "-0", "1 ", "1x". */
static bool parse_i64(const char *s, int64_t *out)
{
    bool neg = false;
    if (*s == '-') { neg = true; s++; }
    if (*s == '\0') return false;
    if (*s == '0' && s[1] != '\0') return false;   /* leading zeros ("00", "007") */
    if (neg && *s == '0') return false;           /* "-0" (and "-0..." is covered above) */

    const uint64_t limit = neg ? ((uint64_t)1 << 63) : (uint64_t)INT64_MAX;
    uint64_t v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return false;
        uint64_t digit = (uint64_t)(*s - '0');
        if (v > (limit - digit) / 10) return false;
        v = v * 10 + digit;
    }
    *out = neg ? (int64_t)(0 - v) : (int64_t)v;
    return true;
}

/*  service interface  */

struct dec dec_get(const struct env_ops *e, const char *ns, const char *name)
{
    struct dec d = {0};
    if (e == NULL || name == NULL) return d;

    char ns_buf[ENV_NAME_MAX + 1];
    if (!build_ns(ns, ns_buf, sizeof ns_buf)) return d;

    /* Metadata first: reject foreign or mistyped entries before parsing. */
    struct env_stat st;
    if (e->stat(ns_buf, name, &st) != ENV_OK) return d;

    if (env_mask_type(st.mask) != ENV_TYPE_INT) return d;
    uint8_t owner = env_mask_owner(st.mask);
    if (owner != BUS_VM16 && owner != BUS_PROXY) return d;

    uint16_t priv  = env_mask_priv(st.mask);
    uint8_t scale  = (uint8_t)DEC_P_SCALE(priv);
    uint8_t round  = (uint8_t)DEC_P_ROUND(priv);
    uint8_t flags  = (uint8_t)DEC_P_FLAGS(priv);
    if (scale > DEC_MAX_SCALE) return d;
    if (!rnd_valid((enum dec_round)round)) return d;
    /* Only DEC_F_SIGNED can be legitimately written by dec_set;
     * anything else is a corrupt or foreign entry, so refuse to decode it. */
    if (flags & ~(uint8_t)DEC_F_SIGNED) return d;

    /* 20 digits + sign + NUL = 22; anything longer is not an int64 anyway
     * and comes back as ENV_BUFFER_TOO_SMALL. */
    char buf[32];
    size_t len = 0;
    if (e->peek(ns_buf, name, buf, sizeof buf, &len) != ENV_OK) return d;

    int64_t v;
    if (!parse_i64(buf, &v)) return d;

    d.mag = v;
    d.scale = scale;
    d.round = round;
    d.flags = flags;
    d.ok = true;
    return d;
}

bool dec_set(const struct env_ops *e, const char *ns, const char *name,
             int64_t mag, int8_t scale, const struct dec_signature *sig, uint8_t importance)
{
    if (e == NULL || name == NULL) return false;

    uint8_t final_scale = (scale < 0)
        ? (sig ? sig->default_scale : (uint8_t)DEC_DEFAULT_SCALE)
        : (uint8_t)scale;
    uint8_t final_round = sig ? sig->default_round : (uint8_t)DEC_RND_TRUNC;
    uint8_t final_flags = sig ? sig->default_flags : (uint8_t)DEC_F_SIGNED;

    if (final_scale > DEC_MAX_SCALE)                      return false;
    if (!rnd_valid((enum dec_round)final_round))          return false;
    /* DEC_F_SATURATE is not implemented: refuse to store behavior we cannot
     * honor, rather than silently rounding as if it were strict overflow. */
    if (final_flags & ~(uint8_t)DEC_F_SIGNED)              return false;
    if (importance > ENV_IMPORTANCE_MAX) importance = ENV_IMPORTANCE_MAX;

    char ns_buf[ENV_NAME_MAX + 1];
    if (!build_ns(ns, ns_buf, sizeof ns_buf)) return false;

    char buf[32];
    int i = (int)sizeof(buf) - 1;
    buf[i] = '\0';
    bool neg = mag < 0;
    uint64_t uv = neg ? (0 - (uint64_t)mag) : (uint64_t)mag;
    if (uv == 0) buf[--i] = '0';
    while (uv > 0) { buf[--i] = (char)('0' + (uv % 10)); uv /= 10; }
    if (neg) buf[--i] = '-';

    /* Decimal ENV symbol contract:
     * - Expects ENV_TYPE_INT types
     * - Never expects ENV_FLAG_CACHEABLE
     * - Always expects BUS_VM16 for bus->env transactions
     */
    uint16_t priv = DEC_P_MAKE(final_scale, final_round, final_flags);
    uint32_t mask = env_mask_build(BUS_VM16, ENV_TYPE_INT, 0, priv);

    return e->set(ns_buf, name, &buf[i], 0, 0, importance, mask) == ENV_OK;
}

/*  arithmetic  */

bool dec_align(struct dec *a, struct dec *b, uint8_t *common)
{
    if (a == NULL || b == NULL || common == NULL) return false;
    if (a->scale > DEC_MAX_SCALE || b->scale > DEC_MAX_SCALE) return false;

    uint8_t s = (a->scale > b->scale) ? a->scale : b->scale;
    int64_t am, bm;
    if (!upscale_i64(a->mag, a->scale, s, &am)) return false;
    if (!upscale_i64(b->mag, b->scale, s, &bm)) return false;

    a->mag = am; a->scale = s;
    b->mag = bm; b->scale = s;
    *common = s;
    return true;
}

static struct dec addsub(struct dec a, struct dec b, bool subtract,
                         enum dec_round rnd, uint8_t rs)
{
    struct dec r = {0};
    r.round = (uint8_t)rnd;
    if (!dec_valid(&a) || !dec_valid(&b) || rs > DEC_MAX_SCALE || !rnd_valid(rnd)) return r;

    uint8_t common = (a.scale > b.scale) ? a.scale : b.scale;
    bool sa, sb; u128 ma, mb;
    if (!align_mag(&a, common, &sa, &ma)) return r;
    if (!align_mag(&b, common, &sb, &mb)) return r;
    if (subtract) sb = !sb;

    bool sr; u128 mr;
    signed_sum(sa, ma, sb, mb, &sr, &mr);

    unsigned up   = rs > common ? (unsigned)(rs - common) : 0;
    unsigned down = common > rs ? (unsigned)(common - rs) : 0;
    int64_t v;
    if (!scale_signed(sr, mr, up, down, rnd, &v)) return r;

    r.mag = v; r.scale = rs; r.ok = true;
    return r;
}

struct dec dec_add(struct dec a, struct dec b, enum dec_round rnd, uint8_t result_scale)
{
    return addsub(a, b, false, rnd, result_scale);
}

struct dec dec_sub(struct dec a, struct dec b, enum dec_round rnd, uint8_t result_scale)
{
    return addsub(a, b, true, rnd, result_scale);
}

struct dec dec_mul(struct dec a, struct dec b, enum dec_round rnd, uint8_t result_scale)
{
    struct dec r = {0};
    r.round = (uint8_t)rnd;
    if (!dec_valid(&a) || !dec_valid(&b) || result_scale > DEC_MAX_SCALE || !rnd_valid(rnd)) return r;

    bool na, nb;
    u128 ma = mag_of(a.mag, &na);
    u128 mb = mag_of(b.mag, &nb);
    u128 prod = ma * mb;                          /* < 2^126, cannot overflow */

    unsigned natural = (unsigned)a.scale + (unsigned)b.scale;   /* <= 36, no uint8 wrap */
    unsigned up   = result_scale > natural ? result_scale - natural : 0;
    unsigned down = natural > result_scale ? natural - result_scale : 0;

    int64_t v;
    if (!scale_signed(na != nb, prod, up, down, rnd, &v)) return r;

    r.mag = v; r.scale = result_scale; r.ok = true;
    return r;
}

struct dec dec_div(struct dec a, struct dec b, enum dec_round rnd, uint8_t result_scale)
{
    struct dec r = {0};
    r.round = (uint8_t)rnd;
    if (!dec_valid(&a) || !dec_valid(&b) || result_scale > DEC_MAX_SCALE || !rnd_valid(rnd)) return r;
    if (b.mag == 0) return r;

    /* (am/10^as) / (bm/10^bs) at scale rs  =  am * 10^(rs + bs - as) / bm
     * The divisor stays < 2^63, so a 10^k scaling overflow on the dividend
     * implies a quotient >= 2^65: rejection always means "does not fit". */
    int k = (int)result_scale + (int)b.scale - (int)a.scale;
    unsigned up   = k > 0 ? (unsigned)k  : 0;
    unsigned down = k < 0 ? (unsigned)-k : 0;

    bool na, nb;
    u128 num = mag_of(a.mag, &na);
    u128 den = mag_of(b.mag, &nb);
    if (!u128_mul_pow10(&num, up))   return r;
    if (!u128_mul_pow10(&den, down)) return r;

    int64_t v;
    if (!round_div(num, den, na != nb, rnd, &v)) return r;

    r.mag = v; r.scale = result_scale; r.ok = true;
    return r;
}

struct dec dec_mod(struct dec a, struct dec b, enum dec_round rnd, uint8_t result_scale)
{
    struct dec r = {0};
    r.round = (uint8_t)rnd;
    if (!dec_valid(&a) || !dec_valid(&b) || result_scale > DEC_MAX_SCALE || !rnd_valid(rnd)) return r;
    if (b.mag == 0) return r;

    uint8_t common = (a.scale > b.scale) ? a.scale : b.scale;
    bool sa, sb; u128 ma, mb;
    if (!align_mag(&a, common, &sa, &ma)) return r;
    if (!align_mag(&b, common, &sb, &mb)) return r;

    u128 rem;
    (void)udivmod128(ma, mb, &rem);               /* sign follows the dividend */

    unsigned up   = result_scale > common ? (unsigned)(result_scale - common) : 0;
    unsigned down = common > result_scale ? (unsigned)(common - result_scale) : 0;
    int64_t v;
    if (!scale_signed(sa && rem != 0, rem, up, down, rnd, &v)) return r;

    r.mag = v; r.scale = result_scale; r.ok = true;
    return r;
}

/* CMP operations */
int dec_cmp(struct dec a, struct dec b)
{
    if (!dec_valid(&a) || !dec_valid(&b)) return -2;

    uint8_t common;
    struct dec a_copy = a;
    struct dec b_copy = b;

    if (!dec_align(&a_copy, &b_copy, &common)) {
        /* Alignment failed because scaling up exceeded int64_t bounds.
         * Fall back to u128 scale-cross comparison to compare precisely. */
        bool na, nb;
        u128 ma = mag_of(a.mag, &na);
        u128 mb = mag_of(b.mag, &nb);

        if (na != nb) { return na ? -1 : 1; /* Negative is always less than positive */ }

        u128 ma_scaled = ma;
        u128 mb_scaled = mb;
        if (a.scale < b.scale) {
            u128_mul_pow10(&ma_scaled, (unsigned)(b.scale - a.scale));
        } else if (b.scale < a.scale) {
            u128_mul_pow10(&mb_scaled, (unsigned)(a.scale - b.scale));
        }

        int res = (ma_scaled < mb_scaled) ? -1 : ((ma_scaled > mb_scaled) ? 1 : 0);
        return na ? -res : res;
    }

    /* Successfully aligned and fits within int64 bounds */
    if (a_copy.mag < b_copy.mag) return -1;
    if (a_copy.mag > b_copy.mag) return 1;
    return 0;
}
