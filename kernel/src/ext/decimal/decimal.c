/* dec_toolkit, rev 0.1
 * Experimental 'augmented' decimal type using of env + mask.h
 * to provide floating point arithmetic and operations.
 *
 * Numeric story end-to-end in C:
 *   - payload: integer, stored in env `ENV_TYPE_INT`
 *   - scale:   carried in mask PRIVATE[15:8]
 *   - rounding/flags in PRIVATE[7:4] / PRIVATE[3:0]
 *   - add/sub/compare align to max(scale); mul scale-adds; div rounds
 *   - overflow is an error, never a wrap
 *
 * Benefits:
 * - Uses no SSE, no FPU or x87
 * - All arithmetic is int64_t with explicit overflow checks (widening to __int128 when the compiler has it)
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <core/version.h>
#include <lib/string.h>
#include <core/dev/env.h>
#include <core/mask.h>
#include <core/kprintf.h>
#include <core/cmd.h>
#include <core/ext.h>
#include <core/bus.h>

/* PRIVATE descriptor layout (see spec) */
#define DEC_P_SCALE(priv)   (((priv) >> 8) & 0xFF)
#define DEC_P_ROUND(priv)   (((priv) >> 4) & 0x0F)
#define DEC_P_FLAGS(priv)   (((priv)     ) & 0x0F)
#define DEC_P_MAKE(s,r,f)   ((((uint16_t)(s) & 0xFF) << 8) | \
                             (((uint16_t)(r) & 0x0F) << 4) | \
                              ((uint16_t)(f) & 0x0F))

#define DEC_F_SIGNED        0x1
#define DEC_F_SATURATE      0x2

enum dec_round {
    DEC_RND_TRUNC   = 0,
    DEC_RND_HALF_UP = 1,
    DEC_RND_HALF_EVEN = 2,
};

/* A decoded decimal: integer payload + scale + flags. */
struct dec {
    int64_t  mag;      /* signed integer payload */
    uint8_t  scale;    /* fractional digits */
    uint8_t  flags;    /* DEC_F_* */
    uint8_t  round;    /* DEC_RND_* */
    bool     ok;       /* false if parse/arith failed */
};

/*  scale arithmetic helpers  */

static const int64_t POW10[19] = {
    1LL, 10LL, 100LL, 1000LL, 10000LL, 100000LL, 1000000LL, 10000000LL,
    100000000LL, 1000000000LL, 10000000000LL, 100000000000LL,
    1000000000000LL, 10000000000000LL, 100000000000000LL,
    1000000000000000LL, 10000000000000000LL, 100000000000000000LL,
    1000000000000000000LL
};

/* multiply with overflow check; returns false on overflow */
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

/* rescale x from `from` to `to` (to >= from), exact */
static bool rescale_up(int64_t x, uint8_t from, uint8_t to, int64_t *out) {
    if (to < from || to > 18) return false;
    int64_t factor = POW10[to - from];
    return mul_i64(x, factor, out);
}

/* rescale x from `from` to `to` (to <= from), applying rounding */
static bool rescale_down(int64_t x, uint8_t from, uint8_t to, enum dec_round rnd,
                         int64_t *out) {
    if (to > from || from > 18) return false;
    int64_t divisor = POW10[from - to];
    if (divisor == 0) return false;
    int64_t q = x / divisor;
    int64_t rem = x % divisor;

    if (rnd == DEC_RND_TRUNC) { *out = q; return true; }

    int64_t half = divisor / 2;
    if (rem < 0) { rem = -rem; }

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

/* parse numbers  */

/* Non-negative integer: decimal, or hex with a 0x prefix.
 * Local copy: env_toolkit's parse_num is static to that TU, not exported. */
static bool parse_num(const char *s, uint64_t *out) {
    if (s == NULL || *s == '\0')
        return false;

    uint64_t base = 10;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s += 2;
        if (*s == '\0')
            return false;
    }

    uint64_t v = 0;
    for (; *s != '\0'; s++) {
        uint64_t d;
        if (*s >= '0' && *s <= '9')                   d = (uint64_t)(*s - '0');
        else if (base == 16 && *s >= 'a' && *s <= 'f') d = (uint64_t)(*s - 'a') + 10u;
        else if (base == 16 && *s >= 'A' && *s <= 'F') d = (uint64_t)(*s - 'A') + 10u;
        else return false;

        if (v > (UINT64_MAX - d) / base)
            return false;   /* overflow */
        v = v * base + d;
    }
    *out = v;
    return true;
}

/*  decode from env  */

/* Fetch `dec.<id>.<name>`, parse the integer payload, pull scale/flags
 * out of the entry's mask, and return a decoded struct. */
static struct dec dec_load(const struct env_ops *e, const char *id, const char *name) {
    struct dec d = {0};
    d.ok = false;
    if (e == NULL || id == NULL || name == NULL) return d;

    char ns[ENV_NAME_MAX + 1];
    /* ns = "dec." + id */
    size_t n = 0;
    const char *p = "dec.";
    while (*p && n < ENV_NAME_MAX) ns[n++] = *p++;
    while (*id && n < ENV_NAME_MAX) ns[n++] = *id++;
    ns[n] = '\0';

    char buf[ENV_VALUE_MAX + 1];
    size_t len = 0;
    if (e->peek(ns, name, buf, sizeof buf, &len) != ENV_OK) return d;

    struct env_stat st;
    if (e->stat(ns, name, &st) != ENV_OK) return d;

    /* scale/round/flags live in PRIVATE */
    uint16_t priv = env_mask_priv(st.mask);
    d.scale = DEC_P_SCALE(priv);
    d.round = DEC_P_ROUND(priv);
    d.flags = DEC_P_FLAGS(priv);

    /* parse decimal integer payload (optionally negative) */
    const char *s = buf;
    bool neg = false;
    if (*s == '-') { neg = true; s++; }
    if (*s == '\0') return d;

    int64_t v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return d;
        int64_t nv;
        if (!mul_i64(v, 10, &nv)) return d;
        int64_t nv2;
        if (!add_i64(nv, *s - '0', &nv2)) return d;
        v = nv2;
    }
    d.mag = neg ? -v : v;
    d.ok = true;
    return d;
}

/*  encode to env  */

static bool dec_store(const struct env_ops *e, const char *id, const char *name,
                      struct dec d, uint8_t importance) {
    if (e == NULL || !d.ok) return false;

    char ns[ENV_NAME_MAX + 1];
    size_t n = 0;
    const char *p = "dec.";
    while (*p && n < ENV_NAME_MAX) ns[n++] = *p++;
    while (*id && n < ENV_NAME_MAX) ns[n++] = *id++;
    ns[n] = '\0';

    /* format payload as signed decimal */
    char buf[32];
    int i = (int)sizeof(buf) - 1;
    buf[i] = '\0';
    int64_t v = d.mag;
    bool neg = v < 0;
    uint64_t uv = neg ? (uint64_t)(-(v + 1)) + 1u : (uint64_t)v;
    if (uv == 0) buf[--i] = '0';
    while (uv > 0) { buf[--i] = (char)('0' + (uv % 10)); uv /= 10; }
    if (neg) buf[--i] = '-';

    /* OWNER: Interpreter id is safe to use for this */
    uint16_t priv = DEC_P_MAKE(d.scale, d.round, d.flags);
    uint32_t mask = env_mask_build(BUS_INTPR, ENV_TYPE_INT, ENV_FLAG_CACHEABLE, priv);

    /* overwrite-in-place; the caller (extension) owns BUS_INTPR */
    int r = e->set(ns, name, &buf[i], 0, 0, importance, mask);
    return r == ENV_OK;
}

/*  arithmetic  */

/* Align two operands to max(scale); returns the common scale. */
static bool dec_align(struct dec *a, struct dec *b, uint8_t *common) {
    uint8_t s = (a->scale > b->scale) ? a->scale : b->scale;
    int64_t am, bm;
    if (!rescale_up(a->mag, a->scale, s, &am)) return false;
    if (!rescale_up(b->mag, b->scale, s, &bm)) return false;
    a->mag = am; a->scale = s;
    b->mag = bm; b->scale = s;
    *common = s;
    return true;
}

static struct dec dec_add(struct dec a, struct dec b, enum dec_round rnd, uint8_t result_scale) {
    struct dec r = {0}; r.ok = false; r.round = rnd;
    if (!a.ok || !b.ok) return r;
    uint8_t common;
    if (!dec_align(&a, &b, &common)) return r;
    int64_t s;
    if (!add_i64(a.mag, b.mag, &s)) return r;
    /* optionally narrow to result_scale (exact if result_scale >= common) */
    if (result_scale < common) {
        if (!rescale_down(s, common, result_scale, rnd, &s)) return r;
        common = result_scale;
    }
    r.mag = s; r.scale = common; r.ok = true;
    return r;
}

static struct dec dec_sub(struct dec a, struct dec b, enum dec_round rnd, uint8_t result_scale) {
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

static struct dec dec_mul(struct dec a, struct dec b, enum dec_round rnd, uint8_t result_scale) {
    struct dec r = {0}; r.ok = false; r.round = rnd;
    if (!a.ok || !b.ok) return r;
    int64_t prod;
    if (!mul_i64(a.mag, b.mag, &prod)) return r;
    uint8_t natural = (uint8_t)(a.scale + b.scale);
    if (natural > 18) return r;   /* would need >18 digits of scale: reject */
    /* bring product down to result_scale (rounding applies here) */
    if (!rescale_down(prod, natural, result_scale, rnd, &prod)) return r;
    r.mag = prod; r.scale = result_scale; r.ok = true;
    return r;
}

static struct dec dec_div(struct dec a, struct dec b, enum dec_round rnd, uint8_t result_scale) {
    struct dec r = {0}; r.ok = false; r.round = rnd;
    if (!a.ok || !b.ok || b.mag == 0) return r;
    /* bring both to a common scale, then do integer division with the
     * target scale baked into the numerator. */
    uint8_t common = (a.scale > b.scale) ? a.scale : b.scale;
    int64_t am, bm;
    if (!rescale_up(a.mag, a.scale, common, &am)) return r;
    if (!rescale_up(b.mag, b.scale, common, &bm)) return r;
    /* result.mag = (am / bm) * 10^result_scale */
    int64_t scaled;
    if (result_scale > common) {
        /* am * 10^(result_scale-common) / bm */
        if (!rescale_up(am, common, result_scale, &am)) return r;
        scaled = am / bm;
    } else {
        /* (am / bm) then rescale_down */
        int64_t q = am / bm;
        if (!rescale_down(q, common, result_scale, rnd, &scaled)) return r;
    }
    r.mag = scaled; r.scale = result_scale; r.ok = true;
    return r;
}

static struct dec dec_mod(struct dec a, struct dec b, enum dec_round rnd, uint8_t result_scale) {
    struct dec r = {0}; r.ok = false; r.round = rnd;
    if (!a.ok || !b.ok || b.mag == 0) return r;
    uint8_t common;
    if (!dec_align(&a, &b, &common)) return r;
    int64_t m = a.mag % b.mag;      /* remainder at `common` scale */
    if (result_scale < common) {
        if (!rescale_down(m, common, result_scale, rnd, &m)) return r;
        common = result_scale;
    }
    r.mag = m; r.scale = common; r.ok = true;
    return r;
}

/*  shell front-end  */

static const struct env_ops *env_or_null(void) {
    const struct env_ops *e = bus_env();
    if (e == NULL) kprintf("dec: no env device\n");
    return e;
}

/* dec set <id> <name> <integer> <scale> [round] [flags]
 *   The integer payload is the raw value; the *value* is payload / 10^scale.
 *   e.g. "dec set A x 150 2" stores 1.50. */
int sub_dec_set(int argc, const char *const *argv) {
    if (argc < 4 || argc > 6) {
        kprintf("usage: dec set <id> <name> <int> <scale> [round] [flags]\n");
        return CMD_USAGE;
    }
    const struct env_ops *e = env_or_null();
    if (e == NULL) return 1;

    /* parse integer payload (allow leading '-') */
    const char *s = argv[2];
    bool neg = false;
    if (*s == '-') { neg = true; s++; }
    int64_t v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') { kprintf("dec set: bad integer\n"); return CMD_USAGE; }
        int64_t nv;
        if (!mul_i64(v, 10, &nv)) { kprintf("dec set: integer overflow\n"); return 1; }
        int64_t nv2;
        if (!add_i64(nv, *s - '0', &nv2)) { kprintf("dec set: integer overflow\n"); return 1; }
        v = nv2;
    }
    if (neg) v = -v;

    uint8_t scale = 0, rnd = DEC_RND_TRUNC, flags = DEC_F_SIGNED;
    {
        uint64_t tmp;
        if (!parse_num(argv[3], &tmp) || tmp > 18) { kprintf("dec set: scale 0..18\n"); return CMD_USAGE; }
        scale = (uint8_t)tmp;
    }
    if (argc >= 5) { uint64_t t; if (!parse_num(argv[4], &t) || t > 2) { kprintf("dec set: round 0..2\n"); return CMD_USAGE; } rnd = (uint8_t)t; }
    if (argc >= 6) { uint64_t t; if (!parse_num(argv[5], &t) || t > 15) { kprintf("dec set: flags 0..15\n"); return CMD_USAGE; } flags = (uint8_t)t; }

    struct dec d = { .mag = v, .scale = scale, .round = rnd, .flags = flags, .ok = true };
    if (!dec_store(e, argv[0], argv[1], d, ENV_IMPORTANCE_DEFAULT)) {
        kprintf("dec set: store failed\n"); return 1;
    }
    kprintf("dec set: %s.%s = %lld x 10^-%u (round=%u flags=0x%x)\n",
            argv[0], argv[1], (long long)v, scale, rnd, flags);
    return CMD_OK;
}

static int sub_dec_show(int argc, const char *const *argv) {
    if (argc != 2) { kprintf("usage: dec show <id> <name>\n"); return CMD_USAGE; }
    const struct env_ops *e = env_or_null();
    if (e == NULL) return 1;
    struct dec d = dec_load(e, argv[0], argv[1]);
    if (!d.ok) { kprintf("dec show: not found or malformed\n"); return 1; }
    kprintf("dec show: %s.%s = %lld x 10^-%u  (scale=%u round=%u flags=0x%x)\n",
            argv[0], argv[1], (long long)d.mag, d.scale, d.scale, d.round, d.flags);
    return CMD_OK;
}

/* dec op <id> <a> <op> <b> <dst> [result_scale] [round]
 *   Reads a and b, applies op, writes to dst. result_scale defaults to max(scales). */
static int sub_dec_op(int argc, const char *const *argv) {
    if (argc < 5 || argc > 7) {
        kprintf("usage: dec op <id> <a> <op> <b> <dst> [result_scale] [round]\n");
        kprintf("       op = add|sub|mul|div|mod\n");
        return CMD_USAGE;
    }
    const struct env_ops *e = env_or_null();
    if (e == NULL) return 1;

    struct dec a = dec_load(e, argv[0], argv[1]);
    struct dec b = dec_load(e, argv[0], argv[3]);
    if (!a.ok) { kprintf("dec op: lhs not found\n"); return 1; }
    if (!b.ok) { kprintf("dec op: rhs not found\n"); return 1; }

    enum dec_round rnd = DEC_RND_TRUNC;
    if (argc >= 7) { uint64_t t; if (!parse_num(argv[6], &t) || t > 2) { kprintf("dec op: round 0..2\n"); return CMD_USAGE; } rnd = (enum dec_round)t; }

    uint8_t rs = (a.scale > b.scale) ? a.scale : b.scale;
    if (argc >= 6) { uint64_t t; if (!parse_num(argv[5], &t) || t > 18) { kprintf("dec op: scale 0..18\n"); return CMD_USAGE; } rs = (uint8_t)t; }

    struct dec r = {0}; r.ok = false;
    const char *op = argv[2];
    if      (strcmp(op, "add") == 0) r = dec_add(a, b, rnd, rs);
    else if (strcmp(op, "sub") == 0) r = dec_sub(a, b, rnd, rs);
    else if (strcmp(op, "mul") == 0) r = dec_mul(a, b, rnd, rs);
    else if (strcmp(op, "div") == 0) r = dec_div(a, b, rnd, rs);
    else if (strcmp(op, "mod") == 0) r = dec_mod(a, b, rnd, rs);
    else { kprintf("dec op: unknown op '%s'\n", op); return CMD_USAGE; }

    if (!r.ok) { kprintf("dec op: arithmetic failed (overflow, div-by-zero, or scale out of range)\n"); return 1; }
    if (!dec_store(e, argv[0], argv[4], r, ENV_IMPORTANCE_DEFAULT)) {
        kprintf("dec op: store failed\n"); return 1;
    }
    kprintf("dec op: %s.%s = %lld x 10^-%u\n",
            argv[0], argv[4], (long long)r.mag, r.scale);
    return CMD_OK;
}

/* dec cmp <id> <a> <b>: prints -1/0/1 after alignment */
static int sub_dec_cmp(int argc, const char *const *argv) {
    if (argc != 3) { kprintf("usage: dec cmp <id> <a> <b>\n"); return CMD_USAGE; }
    const struct env_ops *e = env_or_null();
    if (e == NULL) return 1;
    struct dec a = dec_load(e, argv[0], argv[1]);
    struct dec b = dec_load(e, argv[0], argv[2]);
    if (!a.ok || !b.ok) { kprintf("dec cmp: not found\n"); return 1; }
    uint8_t common;
    if (!dec_align(&a, &b, &common)) { kprintf("dec cmp: align overflow\n"); return 1; }
    int r = (a.mag < b.mag) ? -1 : (a.mag > b.mag) ? 1 : 0;
    kprintf("dec cmp: %d (a=%lld b=%lld at scale %u)\n", r, (long long)a.mag, (long long)b.mag, common);
    return CMD_OK;
}

static int sub_dec_help(int argc, const char *const *argv) {
    (void)argc; (void)argv;
    kprintf(
        "dec commands:\n"
        "  set  <id> <name> <int> <scale> [round] [flags]\n"
        "  show <id> <name>\n"
        "  op   <id> <a> <op> <b> <dst> [scale] [round]\n"
        "       op = add|sub|mul|div|mod\n"
        "  cmp  <id> <a> <b>\n"
        "  help\n"
        "\n"
        "Payload is an integer, value = payload / 10^scale\n"
        "FP metadata is stored in mask bits field, no SSE, no FPU is used.\n");
    return CMD_OK;
}

struct dec_subcmd { const char *name; int (*fn)(int, const char *const *); };
static const struct dec_subcmd subcmds[] = {
    { "set", sub_dec_set }, { "show", sub_dec_show }, { "op", sub_dec_op },
    { "cmp", sub_dec_cmp }, { "help", sub_dec_help }, { NULL, NULL }
};

static int cmd_dec(const struct cmd_args *a) {
    if (a->argc < 1) return sub_dec_help(0, NULL);
    for (const struct dec_subcmd *s = subcmds; s->name; s++)
        if (strcmp(s->name, a->argv[0]) == 0)
            return s->fn(a->argc - 1, a->argv + 1);
    kprintf("dec: unknown subcommand '%s'\n", a->argv[0]);
    return CMD_USAGE;
}
SYM_COMMAND(dec, "<subcmd:str> [args:str...]", "Decimal Tester (env-backed)", cmd_dec);

static int dec_ext_init(void) { return 0; }
SYM_EXTENSION(dec_toolkit, dec_ext_init, EXT_PRIO_APPLET);
