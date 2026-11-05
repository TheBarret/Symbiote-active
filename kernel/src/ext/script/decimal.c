#include <lib/string.h>
#include <core/dev/env.h>
#include <core/mask.h>
#include <core/bus.h>
#include <core/kprintf.h>
#include <ext/script/decimal.h>

static const int64_t POW10[19] = {
    1LL, 10LL, 100LL, 1000LL, 10000LL, 100000LL, 1000000LL, 10000000LL,
    100000000LL, 1000000000LL, 10000000000LL, 100000000000LL,
    1000000000000LL, 10000000000000LL, 100000000000000LL,
    1000000000000000LL, 10000000000000000LL, 100000000000000000LL,
    1000000000000000000LL
};

static bool mul_i64(int64_t a, int64_t b, int64_t *out) {
#if defined(__SIZEOF_INT128__)
    __int128 r = (__int128)a * (__int128)b;
    if (r < INT64_MIN || r > INT64_MAX) return false;
    *out = (int64_t)r;
    return true;
#else
    if (a == 0 || b == 0) { *out = 0; return true; }
    if (a == INT64_MIN && b == -1) return false;
    if (b == INT64_MIN && a == -1) return false;
    int64_t r = a * b;
    if (r / b != a) return false;
    *out = r;
    return true;
#endif
}

static bool add_i64(int64_t a, int64_t b, int64_t *out) {
#if defined(__GNUC__) || defined(__clang__)
    return !__builtin_add_overflow(a, b, out);
#else
    if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b)) return false;
    *out = a + b;
    return true;
#endif
}

static bool sub_i64(int64_t a, int64_t b, int64_t *out) {
#if defined(__GNUC__) || defined(__clang__)
    return !__builtin_sub_overflow(a, b, out);
#else
    if ((b < 0 && a > INT64_MAX + b) || (b > 0 && a < INT64_MIN + b)) return false;
    *out = a - b;
    return true;
#endif
}

static bool rescale_up(int64_t x, uint8_t from, uint8_t to, int64_t *out) {
    if (to < from || to > DEC_MAX_SCALE) return false;
    int64_t factor = POW10[to - from];
    return mul_i64(x, factor, out);
}

static bool rescale_down(int64_t x, uint8_t from, uint8_t to, enum dec_round rnd, int64_t *out) {
    if (to > from || from > DEC_MAX_SCALE) return false;
    int64_t divisor = POW10[from - to];
    if (divisor == 0) return false;
    int64_t q = x / divisor;
    int64_t rem = x % divisor;

    if (rnd == DEC_RND_TRUNC) { *out = q; return true; }

    int64_t half = divisor / 2;
    if (rem < 0) rem = -rem;

    if (rnd == DEC_RND_HALF_UP) {
        if (rem >= half) {
            if (x >= 0) { if (!add_i64(q, 1, &q)) return false; }
            else        { if (!sub_i64(q, 1, &q)) return false; }
        }
    } else if (rnd == DEC_RND_HALF_EVEN) {
        bool round_up = (rem > half) || (rem == half && (q & 1));
        if (round_up) {
            if (x >= 0) { if (!add_i64(q, 1, &q)) return false; }
            else        { if (!sub_i64(q, 1, &q)) return false; }
        }
    }
    *out = q;
    return true;
}

static bool build_ns(const char *ns, char *buf, size_t max_len) {
    size_t n = 0;
    const char *p = "dec.";
    while (*p && n < max_len - 1) buf[n++] = *p++;
    if (ns) {
        while (*ns && n < max_len - 1) buf[n++] = *ns++;
    }
    buf[n] = '\0';
    return true;
}

struct dec dec_get(const struct env_ops *e, const char *ns, const char *name) {
    struct dec d = {0};
    d.ok = false;
    if (e == NULL || name == NULL) return d;

    char ns_buf[ENV_NAME_MAX + 1];
    build_ns(ns, ns_buf, sizeof(ns_buf));

    char buf[ENV_VALUE_MAX + 1];
    size_t len = 0;
    if (e->peek(ns_buf, name, buf, sizeof(buf), &len) != ENV_OK) return d;

    struct env_stat st;
    if (e->stat(ns_buf, name, &st) != ENV_OK) return d;

    uint16_t priv = env_mask_priv(st.mask);
    d.scale = DEC_P_SCALE(priv);
    d.round = DEC_P_ROUND(priv);
    d.flags = DEC_P_FLAGS(priv);

    const char *s = buf;
    bool neg = false;
    if (*s == '-') { neg = true; s++; }
    if (*s == '\0') return d;

    int64_t v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return d;
        int64_t nv, nv2;
        if (!mul_i64(v, 10, &nv)) return d;
        if (!add_i64(nv, *s - '0', &nv2)) return d;
        v = nv2;
    }
    d.mag = neg ? -v : v;
    d.ok = true;
    return d;
}

bool dec_set(const struct env_ops *e, const char *ns, const char *name,
             int64_t mag, int8_t scale, const struct dec_signature *sig, uint8_t importance) {
    if (e == NULL || name == NULL) return false;

    uint8_t final_scale = (scale < 0) ? (sig ? sig->default_scale : DEC_DEFAULT_SCALE) : (uint8_t)scale;
    uint8_t final_round = sig ? sig->default_round : DEC_RND_TRUNC;
    uint8_t final_flags = sig ? sig->default_flags : DEC_F_SIGNED;

    if (final_scale > DEC_MAX_SCALE) return false;

    char ns_buf[ENV_NAME_MAX + 1];
    build_ns(ns, ns_buf, sizeof(ns_buf));

    char buf[32];
    int i = (int)sizeof(buf) - 1;
    buf[i] = '\0';
    bool neg = mag < 0;
    uint64_t uv = neg ? (uint64_t)(-(mag + 1)) + 1u : (uint64_t)mag;
    if (uv == 0) buf[--i] = '0';
    while (uv > 0) { buf[--i] = (char)('0' + (uv % 10)); uv /= 10; }
    if (neg) buf[--i] = '-';

    uint16_t priv = DEC_P_MAKE(final_scale, final_round, final_flags);
    uint32_t mask = env_mask_build(BUS_INTPR, ENV_TYPE_INT, ENV_FLAG_CACHEABLE, priv);

    int r = e->set(ns_buf, name, &buf[i], 0, 0, importance, mask);
    return r == ENV_OK;
}

bool dec_align(struct dec *a, struct dec *b, uint8_t *common) {
    uint8_t s = (a->scale > b->scale) ? a->scale : b->scale;
    int64_t am, bm;
    if (!rescale_up(a->mag, a->scale, s, &am)) return false;
    if (!rescale_up(b->mag, b->scale, s, &bm)) return false;
    a->mag = am; a->scale = s;
    b->mag = bm; b->scale = s;
    *common = s;
    return true;
}

struct dec dec_add(struct dec a, struct dec b, enum dec_round rnd, uint8_t result_scale) {
    struct dec r = {0}; r.ok = false; r.round = rnd;
    if (!a.ok || !b.ok) return r;
    uint8_t common;
    if (!dec_align(&a, &b, &common)) return r;
    int64_t s;
    if (!add_i64(a.mag, b.mag, &s)) return r;
    if (result_scale < common) {
        if (!rescale_down(s, common, result_scale, rnd, &s)) return r;
        common = result_scale;
    }
    r.mag = s; r.scale = common; r.ok = true;
    return r;
}

struct dec dec_sub(struct dec a, struct dec b, enum dec_round rnd, uint8_t result_scale) {
    struct dec r = {0}; r.ok = false; r.round = rnd;
    if (!a.ok || !b.ok) return r;
    uint8_t common;
    if (!dec_align(&a, &b, &common)) return r;
    int64_t s;
    if (!sub_i64(a.mag, b.mag, &s)) return r;
    if (result_scale < common) {
        if (!rescale_down(s, common, result_scale, rnd, &s)) return r;
        common = result_scale;
    }
    r.mag = s; r.scale = common; r.ok = true;
    return r;
}

struct dec dec_mul(struct dec a, struct dec b, enum dec_round rnd, uint8_t result_scale) {
    struct dec r = {0}; r.ok = false; r.round = rnd;
    if (!a.ok || !b.ok) return r;
    int64_t prod;
    if (!mul_i64(a.mag, b.mag, &prod)) return r;
    uint8_t natural = (uint8_t)(a.scale + b.scale);
    if (natural > DEC_MAX_SCALE) return r;
    if (!rescale_down(prod, natural, result_scale, rnd, &prod)) return r;
    r.mag = prod; r.scale = result_scale; r.ok = true;
    return r;
}

struct dec dec_div(struct dec a, struct dec b, enum dec_round rnd, uint8_t result_scale) {
    struct dec r = {0}; r.ok = false; r.round = rnd;
    if (!a.ok || !b.ok || b.mag == 0) return r;
    uint8_t common = (a.scale > b.scale) ? a.scale : b.scale;
    int64_t am, bm;
    if (!rescale_up(a.mag, a.scale, common, &am)) return r;
    if (!rescale_up(b.mag, b.scale, common, &bm)) return r;
    int64_t scaled;
    if (result_scale > common) {
        if (!rescale_up(am, common, result_scale, &am)) return r;
        scaled = am / bm;
    } else {
        int64_t q = am / bm;
        if (!rescale_down(q, common, result_scale, rnd, &scaled)) return r;
    }
    r.mag = scaled; r.scale = result_scale; r.ok = true;
    return r;
}

struct dec dec_mod(struct dec a, struct dec b, enum dec_round rnd, uint8_t result_scale) {
    struct dec r = {0}; r.ok = false; r.round = rnd;
    if (!a.ok || !b.ok || b.mag == 0) return r;
    uint8_t common;
    if (!dec_align(&a, &b, &common)) return r;
    int64_t m = a.mag % b.mag;
    if (result_scale < common) {
        if (!rescale_down(m, common, result_scale, rnd, &m)) return r;
        common = result_scale;
    }
    r.mag = m; r.scale = common; r.ok = true;
    return r;
}

/* tester */
void test_decimal(void) {
    const struct env_ops *env = bus_env();
    if (!env) return;

    struct dec_signature sig = {
        .default_scale   = 4,
        .default_round   = DEC_RND_HALF_EVEN,
        .default_flags   = DEC_F_SIGNED,
        .strict_overflow = true
    };

    /* STORE: 31416 with scale 4 represents exactly 3.1416 */
    dec_set(env, "decimal", "pi", 31416, 4, &sig, ENV_IMPORTANCE_DEFAULT);

    /* FETCH: Retrieve it back */
    struct dec val = dec_get(env, "decimal", "pi");
    if (!val.ok) return;

    /* To print it properly as a decimal expression (Integer / 10^scale): */
    int64_t divisor = 1;
    for (uint8_t i = 0; i < val.scale; i++) divisor *= 10;

    kprintf("Fetched pi: %lld.%04lld (raw mag: %lld, scale: %u)\n",
            (long long)(val.mag / divisor),
            (long long)(val.mag % divisor < 0 ? -(val.mag % divisor) : val.mag % divisor),
            (long long)val.mag,
            val.scale);
}
