/* env extension: shell front-end for the environment store */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <core/version.h>   /* contains config.h, for default parameters */
#include <lib/string.h>
#include <core/dev/timer.h>
#include <core/dev/env.h>
#include <core/kprintf.h>
#include <core/cmd.h>
#include <core/ext.h>
#include <core/bus.h>


#define LIST_CAP 1000       /* safety bound for enumeration loops */


/* helpers                                                            */


static const struct env_ops *env_or_null(void) {
    const struct env_ops *e = bus_env();
    if (e == NULL)
        kprintf("env: no environment device registered\n");
    return e;
}

static const char *result_name(int r) {
    switch (r) {
    case ENV_OK:                return "OK";
    case ENV_NOT_FOUND:         return "NOT_FOUND";
    case ENV_NO_SPACE:          return "NO_SPACE";
    case ENV_INVALID_ARGS:      return "INVALID_ARGS";
    case ENV_NO_DEVICE:         return "NO_DEVICE";
    case ENV_INTERNAL:          return "INTERNAL";
    case ENV_BUFFER_TOO_SMALL:  return "BUFFER_TOO_SMALL";
    default:                    return "UNDEFINED";
    }
}

/* Non-negative integer: decimal, or hex with a 0x prefix. */
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

/* The ms <-> ticks conversions come from timer.h (ms_to_ticks_ceil and friends).
 * Only the null-safe wrappers that need the bus lookup live here, because they have to cope with "no timer device attached". */

static uint64_t ticks_to_ms(uint64_t ticks) {
    const struct timer_ops *t = bus_timer();
    uint64_t hz = (t != NULL) ? t->hz() : 0;
    if (hz == 0)
        return ticks;       /* no timer: report raw ticks */
    return (ticks / hz) * 1000u + ((ticks % hz) * 1000u) / hz;
}

static uint64_t now_ticks(void) {
    const struct timer_ops *t = bus_timer();
    return (t != NULL) ? t->ticks() : 0;
}


/* subcommand handlers */
/*
 * All take argc/argv AFTER the subcommand name.
 * argv[i] is the i-th argument, the array and the strings are owned by the shell; we only read them. Return CMD_OK, CMD_USAGE, or 1.
 */


/* env set <ns> <name> <value> [access] [ttl_ms] [importance] [mask] */
static int sub_set(int argc, const char *const *argv) {
    if (argc < 3 || argc > 7) {
        kprintf("usage: env set <ns> <name> <value> "
                "[access] [ttl_ms] [importance] [mask]\n");
        return CMD_USAGE;
    }

    const struct env_ops *e = env_or_null();
    if (e == NULL)
        return 1;

    uint32_t access     = 0;
    uint64_t ttl_ms     = 0;
    uint32_t importance = ENV_IMPORTANCE_DEFAULT;
    uint32_t mask       = ENV_MASK_DEFAULT;

    if (argc >= 4) {
        uint64_t v;
        if (!parse_num(argv[3], &v) || v > UINT32_MAX) {
            kprintf("env set: access must be a 32-bit unsigned integer\n");
            return CMD_USAGE;
        }
        access = (uint32_t)v;
    }
    if (argc >= 5) {
        if (!parse_num(argv[4], &ttl_ms)) {
            kprintf("env set: ttl_ms must be a non-negative integer\n");
            return CMD_USAGE;
        }
    }
    if (argc >= 6) {
        uint64_t v;
        if (!parse_num(argv[5], &v) || v > ENV_IMPORTANCE_MAX) {
            kprintf("env set: importance must be 0..%u\n",
                    (unsigned)ENV_IMPORTANCE_MAX);
            return CMD_USAGE;
        }
        importance = (uint32_t)v;
    }
    if (argc >= 7) {
        uint64_t v;
        if (!parse_num(argv[6], &v) || v > UINT32_MAX) {
            kprintf("env set: mask must be a 32-bit integer (decimal or 0x hex)\n");
            return CMD_USAGE;
        }
        mask = (uint32_t)v;
    }

    /* ms_to_ticks_ceil() takes the ms value and returns the tick count directly.
     * It returns 0 for ms == 0 (no TTL) and for the degenerate "timer not yet initialised" case.
     * A non-zero ms value therefore always yields a non-zero tick count when the timer is up. */
    uint64_t ttl_ticks = ms_to_ticks_ceil(ttl_ms);

    int r = e->set(argv[0], argv[1], argv[2], access, ttl_ticks,
                   (uint8_t)importance, mask);
    kprintf("env set: %s (ns=%s name=%s access=%u ttl=%llums imp=%u mask=0x%08x)\n",
            result_name(r), argv[0], argv[1],
            (unsigned)access, (unsigned long long)ttl_ms,
            (unsigned)importance, (unsigned)mask);
    return (r == ENV_OK) ? CMD_OK : 1;
}

/* env get <ns> <name>   |   env peek <ns> <name> */
static int sub_read(int argc, const char *const *argv, bool consume) {
    if (argc != 2) {
        kprintf("usage: env %s <ns> <name>\n", consume ? "get" : "peek");
        return CMD_USAGE;
    }

    const struct env_ops *e = env_or_null();
    if (e == NULL)
        return 1;

    /* The store copies the value out, so the buffer is ours.
     * ENV_VALUE_MAX is the largest value set() accepts, so this can always hold it. */
    char value[ENV_VALUE_MAX + 1];
    size_t len = 0;
    int r = consume ? e->get(argv[0], argv[1], value, sizeof value, &len)
                    : e->peek(argv[0], argv[1], value, sizeof value, &len);

    if (r == ENV_BUFFER_TOO_SMALL) {
        kprintf("env %s: value is %zu bytes, buffer holds %zu (nothing consumed)\n",
                consume ? "get" : "peek", len, sizeof value - 1);
        return 1;
    }
    if (r != ENV_OK) {
        kprintf("env %s: %s\n", consume ? "get" : "peek", result_name(r));
        return 1;
    }

    kprintf("env %s: ns=%s name=%s len=%zu value=\"%s\"\n",
            consume ? "get" : "peek", argv[0], argv[1], len, value);
    return CMD_OK;
}

static int sub_get (int argc, const char *const *argv) { return sub_read(argc, argv, true);  }
static int sub_peek(int argc, const char *const *argv) { return sub_read(argc, argv, false); }

/* env unset <ns> <name> */
static int sub_unset(int argc, const char *const *argv) {
    if (argc != 2) {
        kprintf("usage: env unset <ns> <name>\n");
        return CMD_USAGE;
    }

    const struct env_ops *e = env_or_null();
    if (e == NULL)
        return 1;

    int r = e->unset(argv[0], argv[1]);
    kprintf("env unset: %s\n", result_name(r));
    return (r == ENV_OK) ? CMD_OK : 1;
}

/* env exist <ns> <name> */
static int sub_exist(int argc, const char *const *argv) {
    if (argc != 2) {
        kprintf("usage: env exist <ns> <name>\n");
        return CMD_USAGE;
    }

    const struct env_ops *e = env_or_null();
    if (e == NULL)
        return 1;

    int r = e->is_exist(argv[0], argv[1]);
    kprintf("env exist: %s\n", (r == ENV_OK) ? "yes" : "no");
    return (r == ENV_OK) ? CMD_OK : 1;
}

/* env stat <ns> <name>: entry metadata, no value, nothing consumed */
static int sub_stat(int argc, const char *const *argv) {
    if (argc != 2) {
        kprintf("usage: env stat <ns> <name>\n");
        return CMD_USAGE;
    }

    const struct env_ops *e = env_or_null();
    if (e == NULL)
        return 1;

    struct env_stat st;
    int r = e->stat(argv[0], argv[1], &st);
    if (r != ENV_OK) {
        kprintf("env stat: %s\n", result_name(r));
        return 1;
    }

    kprintf("env stat: ns=%s name=%s\n", argv[0], argv[1]);
    kprintf("  value_len  : %u\n", (unsigned)st.value_len);

    if (st.access_remaining == 0)
        kprintf("  access     : unlimited\n");
    else
        kprintf("  access     : %u read(s) remaining\n", (unsigned)st.access_remaining);

    if (!st.has_ttl)
        kprintf("  ttl        : none\n");
    else
        kprintf("  ttl        : %llu ms remaining\n",
                (unsigned long long)ticks_to_ms(st.ttl_remaining));

    uint64_t now = now_ticks();
    uint64_t age = (now > st.last_access) ? now - st.last_access : 0;
    kprintf("  last access: %llu ms ago\n", (unsigned long long)ticks_to_ms(age));
    kprintf("  importance : %u / %u (higher = evicted last)\n",
            (unsigned)st.importance, (unsigned)ENV_IMPORTANCE_MAX);
    kprintf("  mask       : 0x%08x\n", (unsigned)st.mask);
    return CMD_OK;
}

/* env list [ns]: one namespace, or everything when no ns is given */
static int sub_list(int argc, const char *const *argv) {
    if (argc > 1) {
        kprintf("usage: env list [ns]\n");
        return CMD_USAGE;
    }

    const struct env_ops *e = env_or_null();
    if (e == NULL)
        return 1;

    size_t iter = 0;
    char ns[ENV_NAME_MAX + 1];
    char name[ENV_NAME_MAX + 1];
    int n = 0;

    for (;;) {
        int r;
        if (argc == 1) {
            r = e->list(argv[0], &iter, name, sizeof name);
            if (r == ENV_OK)
                kprintf("  %s\n", name);
        } else {
            r = e->list_all(&iter, ns, sizeof ns, name, sizeof name);
            if (r == ENV_OK)
                kprintf("  %s:%s\n", ns, name);
        }
        if (r != ENV_OK)
            break;
        if (++n >= LIST_CAP) {
            kprintf("  ... (truncated)\n");
            break;
        }
    }

    if (argc == 1)
        kprintf("env list: %d entries in ns=%s\n", n, argv[0]);
    else
        kprintf("env list: %d entries total\n", n);
    return CMD_OK;
}

/* env find <mask>: entries carrying ALL bits of mask; cross-checked against count_by_mask */
static int sub_find(int argc, const char *const *argv) {
    if (argc != 1) {
        kprintf("usage: env find <mask>   (decimal or 0x hex, non-zero)\n");
        return CMD_USAGE;
    }

    uint64_t m;
    if (!parse_num(argv[0], &m) || m == 0 || m > UINT32_MAX) {
        kprintf("env find: mask must be a non-zero 32-bit integer\n");
        return CMD_USAGE;
    }

    const struct env_ops *e = env_or_null();
    if (e == NULL)
        return 1;

    size_t iter = 0;
    char ns[ENV_NAME_MAX + 1];
    char name[ENV_NAME_MAX + 1];
    int n = 0;
    bool capped = false;

    for (;;) {
        int r = e->list_by_mask((uint32_t)m, &iter, ns, sizeof ns, name, sizeof name);
        if (r != ENV_OK)
            break;
        kprintf("  %s:%s\n", ns, name);
        if (++n >= LIST_CAP) {
            kprintf("  ... (truncated)\n");
            capped = true;
            break;
        }
    }

    kprintf("env find: %d match(es) for mask 0x%08x\n", n, (unsigned)m);

    /* Validation: the two query paths must agree. */
    if (!capped) {
        size_t counted = 0;
        int r = e->count_by_mask((uint32_t)m, &counted);
        if (r != ENV_OK)
            kprintf("env find: count_by_mask: %s\n", result_name(r));
        else if (counted != (size_t)n)
            kprintf("env find: MISMATCH, count_by_mask says %zu\n", counted);
    }
    return CMD_OK;
}

/* env drop <ns> */
static int sub_drop(int argc, const char *const *argv) {
    if (argc != 1) {
        kprintf("usage: env drop <ns>\n");
        return CMD_USAGE;
    }

    const struct env_ops *e = env_or_null();
    if (e == NULL)
        return 1;

    int r = e->drop_namespace(argv[0]);
    kprintf("env drop: %s\n", result_name(r));
    return (r == ENV_OK) ? CMD_OK : 1;
}

/* env stats: device-level snapshot */
static int sub_stats(int argc, const char *const *argv) {
    (void)argc;
    (void)argv;

    const struct env_ops *e = env_or_null();
    if (e == NULL)
        return 1;

    struct env_info in;
    int r = e->info(&in);
    if (r != ENV_OK) {
        kprintf("env stats: %s\n", result_name(r));
        return 1;
    }

    kprintf("env stats:\n");
    kprintf("  slots      : %zu used / %zu\n", in.count, in.capacity);
    kprintf("  bytes      : %zu used / %zu\n", in.used_bytes, in.max_bytes);
    kprintf("  schedular  : %s\n", in.timer_attached ? "attached" : "disabled");
    kprintf("  operations : set=%llu get=%llu peek=%llu unset=%llu\n",
            (unsigned long long)in.n_set, (unsigned long long)in.n_get,
            (unsigned long long)in.n_peek, (unsigned long long)in.n_unset);
    kprintf("  reclaimed  : access=%llu time=%llu space=%llu explicit=%llu\n",
            (unsigned long long)in.n_reclaim_access, (unsigned long long)in.n_reclaim_time,
            (unsigned long long)in.n_reclaim_space,  (unsigned long long)in.n_reclaim_explicit);
    kprintf("  purges     : %llu\n", (unsigned long long)in.n_sweeps);
    return CMD_OK;
}

/* env help */
static int sub_help(int argc, const char *const *argv) {
    (void)argc;
    (void)argv;

    kprintf(
        "env commands:\n"
        "  set   <ns> <name> <value> [access] [ttl_ms] [importance] [mask]\n"
        "  get   <ns> <name>         read, with access budget\n"
        "  peek  <ns> <name>         read, without access budget\n"
        "  stat  <ns> <name>         entry metadata\n"
        "  unset <ns> <name>\n"
        "  exist <ns> <name>\n"
        "  list  [ns]                one namespace, or everything\n"
        "  find  <mask>              entries carrying all bits of mask\n"
        "  drop  <ns>\n"
        "  stats                     device counters and capacity\n"
        "  help\n"
        "\n"
        "Fields:\n"
        "  access      : reads before the entry is reclaimed (0 = unlimited)\n"
        "  ttl_ms      : lifetime in milliseconds (0 = no expiry)\n"
        "  importance  : 0-%u, higher = more valuable, evicted last under space\n"
        "                pressure (default %u)\n"
        "  mask        : free-form classification bits, decimal or 0x hex; env never\n"
        "                interprets it (default 0x%08x)\n",
        (unsigned)ENV_IMPORTANCE_MAX,
        (unsigned)ENV_IMPORTANCE_DEFAULT,
        (unsigned)ENV_MASK_DEFAULT);
    return CMD_OK;
}


/* dispatch                                                           */


struct env_subcmd {
    const char *name;
    int (*fn)(int argc, const char *const *argv);
};

static const struct env_subcmd subcmds[] = {
    { "set",   sub_set   },
    { "get",   sub_get   },
    { "peek",  sub_peek  },
    { "stat",  sub_stat  },
    { "unset", sub_unset },
    { "exist", sub_exist },
    { "list",  sub_list  },
    { "find",  sub_find  },
    { "drop",  sub_drop  },
    { "stats", sub_stats },
    { "help",  sub_help  },
    { NULL,    NULL      },
};

/* Top-level handler. a->argc counts args AFTER "env"; a->argv[0] is the subcommand. */
static int cmd_env(const struct cmd_args *a) {
    if (a->argc < 1)
        return sub_help(0, NULL);

    const char *sub = a->argv[0];
    for (const struct env_subcmd *s = subcmds; s->name != NULL; s++) {
        if (strcmp(s->name, sub) == 0)
            return s->fn(a->argc - 1, a->argv + 1);
    }

    kprintf("env: unknown subcommand '%s' (try 'env help')\n", sub);
    return CMD_USAGE;
}

SYM_COMMAND(env, "<subcmd:str> [args:str...]", "Environment tool", cmd_env);

/* extension greeter */
static int env_ext_init(void) { return 0; }
SYM_EXTENSION(env_toolkit, env_ext_init, EXT_PRIO_APPLET);
