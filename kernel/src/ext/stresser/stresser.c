#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <lib/string.h>
#include <core/dev/timer.h>
#include <core/dev/env.h>
#include <core/kprintf.h>
#include <core/klog.h>
#include <core/cmd.h>
#include <core/ext.h>
#include <core/bus.h>
#include <ext/stresser/stresser.h>

#define STRESSER_DEFAULT_N 100
#define STRESSER_MAX_N     4096

static uint32_t rng_state;

static uint32_t rng(void) {
    uint32_t x = rng_state ? rng_state : 0x9e3779b9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state = x;
    return x;
}

static uint64_t now_ticks(void) {
    const struct timer_ops *t = bus_timer();
    return (t != NULL) ? t->ticks() : 0;
}

/* Parse a non-negative decimal. Returns -1 on empty or non-digit input. */
static int parse_uint(const char *s) {
    if (s == NULL || *s == '\0') return -1;
    int v = 0;
    for (; *s != '\0'; s++) {
        if (*s < '0' || *s > '9') return -1;
        if (v > (STRESSER_MAX_N / 10)) return STRESSER_MAX_N + 1;
        v = v * 10 + (*s - '0');
    }
    return v;
}

int stresser_fill(int n) {
    const struct env_ops *env = bus_env();
    if (env == NULL) {
        kprintf("stresser: no environment device registered\n");
        return -1;
    }

    if (n <= 0) n = STRESSER_DEFAULT_N;
    if (n > STRESSER_MAX_N) n = STRESSER_MAX_N;

    rng_state = (uint32_t)now_ticks() | 1u;

    int ok = 0, fail = 0;

    for (int i = 0; i < n; i++) {
        char name[ENV_NAME_MAX + 1];
        char value[ENV_VALUE_MAX + 1];

        ksnprintf(name, sizeof name, "s%05u", (unsigned)i);

        unsigned vlen = 1u + (rng() % 16u);
        if (vlen > ENV_VALUE_MAX) vlen = ENV_VALUE_MAX;
        for (unsigned j = 0; j < vlen; j++)
            value[j] = (char)('a' + (rng() % 26u));
        value[vlen] = '\0';

        uint32_t mask = 0;
        uint64_t ttl  = 0;
        uint32_t budg = 0;
        uint8_t  imp;

        switch (rng() % 3u) {
        case 0:  mask |= STRESSER_SHORT_TTL; ttl = ms_to_ticks_ceil(1000);  break;
        case 1:  mask |= STRESSER_LONG_TTL;  ttl = ms_to_ticks_ceil(30000); break;
        default: mask |= STRESSER_NO_TTL;    ttl = 0;                       break;
        }

        switch (rng() % 2u) {
        case 0:  mask |= STRESSER_LOW_BUDGET; budg = 1u + (rng() % 2u); break;
        default: mask |= STRESSER_NO_BUDGET;  budg = 0;                  break;
        }

        if (rng() & 1u) { mask |= STRESSER_HIGH_IMP; imp = ENV_IMPORTANCE_MAX; }
        else            { mask |= STRESSER_LOW_IMP;  imp = 0; }

        /* Exercise the flag-enforcement paths: PINNED blocks cross-owner
         * overwrite; SECRET hides from list_all. The current owner is
         * BUS_STRESSOR, so re-running fill() with the same name and same
         * owner will succeed on PINNED entries (owner matches). */
        if (rng() & 1u) mask |= STRESSER_PINNED;
        if (rng() & 1u) mask |= STRESSER_SECRET;

        /* Route the mask's OWNER field to BUS_STRESSOR so PINNED semantics
         * align with "the writer". TYPE and FLAGS are unused; PRIVATE holds
         * the taxonomy bits. */
        uint32_t env_mask = mask;
        /* (mask already uses ENV_FLAG_* bits for PINNED/SECRET and raw
         *  bit positions for taxonomy, so it can be passed as-is to
         *  env->set; the OWNER field is 0, which is fine for a stress ns.) */

        int rc = env->set(STRESSER_NS, name, value, budg, ttl, imp, env_mask);
        if (rc == ENV_OK) ok++;
        else              fail++;
    }

    kprintf("stresser fill: %d ok, %d failed (ns=\"%s\")\n",
            ok, fail, STRESSER_NS);
    return ok;
}

int stresser_clear(void) {
    const struct env_ops *env = bus_env();
    if (env == NULL) {
        kprintf("stresser: no environment device registered\n");
        return -1;
    }
    int rc = env->drop_namespace(STRESSER_NS);
    kprintf("stresser clear: rc=%d (ns=\"%s\")\n", rc, STRESSER_NS);
    return (rc == ENV_OK) ? 0 : rc;
}

/*  command dispatch */

static int cmd_stresser_fn(const struct cmd_args *a) {
    const char *sub = a->argv[0];

    if (strcmp(sub, "fill") == 0) {
        int n = STRESSER_DEFAULT_N;
        if (a->argc >= 2) {
            int parsed = parse_uint(a->argv[1]);
            if (parsed < 0) {
                kprintf("stresser fill: bad count '%s'\n", a->argv[1]);
                return CMD_USAGE;
            }
            n = parsed;
        }
        return (stresser_fill(n) >= 0) ? CMD_OK : 1;
    }

    if (strcmp(sub, "clear") == 0)
        return (stresser_clear() == 0) ? CMD_OK : 1;

    kprintf("stresser: unknown subcommand '%s'\n", sub);
    return CMD_USAGE;
}

SYM_COMMAND(stress, "<cmd:str> [arg:str...]", "Environment stress tool (fill, clear)", cmd_stresser_fn);

/* extension greeter */
static int testers_ext_init(void) { return 0; }
SYM_EXTENSION(test_toolkit, testers_ext_init, EXT_PRIO_APPLET);
