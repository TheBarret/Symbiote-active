/* env extension */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <core/bus.h>
#include <core/cmd.h>
#include <core/ext.h>
#include <core/env.h>
#include <core/kprintf.h>
#include <core/timer.h>
#include <lib/string.h>


/* helpers */


static const struct env_ops *env_or_null(void) {
    const struct env_ops *e = bus_env();
    if (e == NULL)
        kprintf("env: no environment device registered\n");
    return e;
}

static const char *result_name(int r) {
    switch (r) {
    case ENV_OK:               return "OK";
    case ENV_NOT_FOUND:        return "NOT_FOUND";
    case ENV_EXPIRED:          return "EXPIRED";
    case ENV_ACCESS_EXHAUSTED: return "ACCESS_EXHAUSTED";
    case ENV_NO_SPACE:         return "NO_SPACE";
    case ENV_INVALID_ARGS:     return "INVALID_ARGS";
    case ENV_NO_DEVICE:        return "NO_DEVICE";
    case ENV_INTERNAL:         return "INTERNAL";
    default:                   return "UNDEFINED";
    }
}

/* Non-negative decimal only. The spec language gives us strings; we do
 * our own numeric parsing because the spec has no per-position type. */
static bool parse_u64(const char *s, uint64_t *out) {
    if (s == NULL || *s == '\0')
        return false;
    uint64_t v = 0;
    for (const char *p = s; *p != '\0'; p++) {
        if (*p < '0' || *p > '9')
            return false;
        uint64_t nv = v * 10u + (uint64_t)(*p - '0');
        if (nv < v)
            return false;   /* overflow */
        v = nv;
    }
    *out = v;
    return true;
}

/* ms -> timer ticks, rounded up so a 1 ms TTL doesn't collapse to 0. */
static uint64_t ms_to_ticks(uint64_t ms) {
    if (ms == 0)
        return 0;
    const struct timer_ops *t = bus_timer();
    if (t == NULL)
        return 0;
    uint64_t hz = t->hz();
    if (hz == 0)
        return 0;
    return (ms * hz + 999u) / 1000u;
}


/* subcommand handlers */

/* All take argc/argv AFTER the subcommand name.
 * argv[i] is the i-th  argument. The array and the strings are owned by the shell;
 * we  only read them. Return CMD_OK, CMD_USAGE, or 1.
 */


/* env set <ns> <name> <value> [access] [ttl_ms] [importance] */
static int sub_set(int argc, const char *const *argv) {
    if (argc < 3 || argc > 6) {
        kprintf("usage: env set <ns> <name> <value> "
                "[access] [ttl_ms] [importance]\n");
        return CMD_USAGE;
    }

    const struct env_ops *e = env_or_null();
    if (e == NULL)
        return 1;

    uint32_t access     = 0;
    uint64_t ttl_ms     = 0;
    uint32_t importance = ENV_IMPORTANCE_DEFAULT;

    if (argc >= 4) {
        uint64_t v;
        if (!parse_u64(argv[3], &v) || v > UINT32_MAX) {
            kprintf("env set: access must be a 32-bit unsigned integer\n");
            return CMD_USAGE;
        }
        access = (uint32_t)v;
    }
    if (argc >= 5) {
        if (!parse_u64(argv[4], &ttl_ms)) {
            kprintf("env set: ttl_ms must be a non-negative integer\n");
            return CMD_USAGE;
        }
    }
    if (argc >= 6) {
        uint64_t v;
        if (!parse_u64(argv[5], &v) || v > ENV_IMPORTANCE_MAX) {
            kprintf("env set: importance must be 0..%u\n",
                    (unsigned)ENV_IMPORTANCE_MAX);
            return CMD_USAGE;
        }
        importance = (uint32_t)v;
    }

    uint64_t ttl_ticks = ms_to_ticks(ttl_ms);

    int r = e->set(argv[0], argv[1], argv[2], access, ttl_ticks,
                   (uint8_t)importance);
    kprintf("env set: %s (ns=%s name=%s access=%u ttl=%llu imp=%u)\n",
            result_name(r), argv[0], argv[1],
            (unsigned)access, (unsigned long long)ttl_ticks,
            (unsigned)importance);
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

    const char *value = NULL;
    int r = consume ? e->get(argv[0], argv[1], &value)
                    : e->peek(argv[0], argv[1], &value);

    if (r != ENV_OK) {
        kprintf("env %s: %s\n", consume ? "get" : "peek", result_name(r));
        return 1;
    }

    /* value is valid until the next mutating call; we print immediately
     * and never store it, which is exactly the contract's requirement. */
    kprintf("env %s: ns=%s name=%s value=\"%s\"\n",
            consume ? "get" : "peek", argv[0], argv[1], value);
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

/* env list <ns> */
static int sub_list(int argc, const char *const *argv) {
    if (argc != 1) {
        kprintf("usage: env list <ns>\n");
        return CMD_USAGE;
    }

    const struct env_ops *e = env_or_null();
    if (e == NULL)
        return 1;

    size_t iter = 0;
    char name[ENV_NAME_MAX + 1];
    int n = 0;

    for (;;) {
        int r = e->list(argv[0], &iter, name, sizeof name);
        if (r != ENV_OK)
            break;
        kprintf("  %s\n", name);
        if (++n >= 1000) {
            kprintf("  ... (truncated)\n");
            break;
        }
    }

    kprintf("env list: %d entries in ns=%s\n", n, argv[0]);
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

/* env stats */
static int sub_stats(int argc, const char *const *argv) {
    (void)argc;
    (void)argv;
    const struct env_ops *e = env_or_null();
    if (e == NULL)
        return 1;
    kprintf("env stats: not implemented yet!\n");
    return CMD_OK;
}

/* env help */
static int sub_help(int argc, const char *const *argv) {
    (void)argc;
    (void)argv;

    kprintf(
        "env commands:\n"
        "  set   <ns> <name> <value> [access] [ttl_ms] [importance]\n"
        "  get   <ns> <name>\n"
        "  peek  <ns> <name>\n"
        "  unset <ns> <name>\n"
        "  exist <ns> <name>\n"
        "  list  <ns>\n"
        "  drop  <ns>\n"
        "  stats\n"
        "  help\n"
        "\n"
        "budgets:\n"
        "  access      consuming reads remaining; 0 = unlimited (default)\n"
        "  ttl_ms      wall-clock lifetime in milliseconds; 0 = none (default)\n"
        "  importance  0 = keep longest, 10 = evict first, default %u\n"
        "              only matters under space pressure\n",
        (unsigned)ENV_IMPORTANCE_DEFAULT);
    return CMD_OK;
}


/* dispatch                                                            */


struct env_subcmd {
    const char *name;
    int (*fn)(int argc, const char *const *argv);
};

static const struct env_subcmd subcmds[] = {
    { "set",   sub_set   },
    { "get",   sub_get   },
    { "peek",  sub_peek  },
    { "unset", sub_unset },
    { "exist", sub_exist },
    { "list",  sub_list  },
    { "drop",  sub_drop  },
    { "stats", sub_stats },
    { "help",  sub_help  },
    { NULL,    NULL      },
};

/* Top-level handler. a->argc counts args AFTER "env"; a->argv[0] is
 * the subcommand. */
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

SYM_COMMAND(env, "<subcmd:str> [args:str...]", "Volatile data storage, contracted environment variables", cmd_env);

/* extension greeter */
static int env_ext_init(void) { return 0; }
SYM_EXTENSION(env_toolkit, env_ext_init, EXT_PRIO_APPLET);
