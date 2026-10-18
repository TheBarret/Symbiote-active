/* envtool: inspection and stress utilities for the environment store */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <lib/string.h>
#include <core/dev/keyboard.h>
#include <core/dev/console.h>
#include <core/dev/timer.h>
#include <core/dev/env.h>
#include <core/kprintf.h>
#include <core/klog.h>
#include <core/cmd.h>
#include <core/ext.h>
#include <core/bus.h>
#include <core/tui.h>
#include <ext/envtool/envtool.h>

#define LEFT_W_DEFAULT   32
#define MAX_CACHED_ITEMS 64
#define ENVTOOL_DEFAULT_N 100
#define ENVTOOL_MAX_N     4096


/*  TUI viewer state and helpers */


struct env_item {
    char ns[ENV_NAME_MAX + 1];
    char name[ENV_NAME_MAX + 1];
    char value[ENV_VALUE_MAX + 1];
    struct env_stat st;
};

struct envtool_state {
    struct env_item items[MAX_CACHED_ITEMS];
    size_t count;           /* cached items */
    size_t selected;
    size_t top;

    struct env_info info;   /* device snapshot, taken before the walk */
    bool    info_ok;
    uint64_t loaded_at;     /* ticks at load, so ttl can be aged at draw time */
};

static uint64_t ticks_to_ms(uint64_t ticks) {
    const struct timer_ops *t = bus_timer();
    uint64_t hz = (t != NULL) ? t->hz() : 0;
    if (hz == 0)
        return ticks;
    return (ticks / hz) * 1000u + ((ticks % hz) * 1000u) / hz;
}

static uint64_t now_ticks(void) {
    const struct timer_ops *t = bus_timer();
    return (t != NULL) ? t->ticks() : 0;
}

/* Values are arbitrary text; keep control bytes from wrecking the grid. */
static void sanitize(char *s) {
    for (; *s != '\0'; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < 0x20 || c == 0x7f)
            *s = '.';
    }
}

/* Loaders
 *
 * Note: peek() refreshes an entry's last_access and bumps the peek counter, so
 * a reload touches every cached entry once. The device snapshot is therefore
 * taken BEFORE the walk, so the counters shown are the store's own, not ours. */
static void load_env_items(struct envtool_state *s) {
    s->count = 0;
    s->selected = 0;
    s->top = 0;
    s->info_ok = false;

    const struct env_ops *env = bus_env();
    if (env == NULL) return;

    s->info_ok = (env->info(&s->info) == ENV_OK);
    s->loaded_at = now_ticks();

    size_t iter = 0;
    while (s->count < MAX_CACHED_ITEMS) {
        struct env_item *item = &s->items[s->count];

        if (env->list_all(&iter, item->ns, sizeof item->ns,
                          item->name, sizeof item->name) != ENV_OK)
            break;

        /* The entry can expire between enumeration and lookup; skip it then. */
        if (env->stat(item->ns, item->name, &item->st) != ENV_OK)
            continue;

        size_t len = 0;
        if (env->peek(item->ns, item->name, item->value, sizeof item->value, &len) == ENV_OK)
            sanitize(item->value);
        else
            strcpy(item->value, "(unavailable)");

        s->count++;
    }
}


/*  TUI draw */


#define KEY_ATTR  TUI_ATTR(TUI_FG_YELLOW, 0)
#define HEAD_ATTR TUI_ATTR(TUI_FG_CYAN, 0)

#define LINE(attr, ...) \
    do { if (cy < ymax) tui_text(cx, cy, (attr), __VA_ARGS__); cy++; } while (0)

#define FIELD(label, ...) \
    do { \
        if (cy < ymax) { \
            tui_text(cx, cy, KEY_ATTR, "%-13s", (label)); \
            tui_text(cx + 14, cy, TUI_ATTR_DEFAULT, __VA_ARGS__); \
        } \
        cy++; \
    } while (0)

static void draw_detail(const struct envtool_state *s, int cx, int cy, int ymax) {
    const struct env_item *item = &s->items[s->selected];
    const struct env_stat *st = &item->st;

    FIELD("namespace", "%s", item->ns);
    FIELD("name", "%s", item->name);
    FIELD("value", "%s", item->value);
    FIELD("length", "%u bytes", (unsigned)st->value_len);
    cy++;

    FIELD("mask", "0x%08x", (unsigned)st->mask);
    FIELD("eviction", "%u / %u", (unsigned)st->importance, (unsigned)ENV_IMPORTANCE_MAX);

    if (st->access_remaining == 0)
        FIELD("access", "unlimited");
    else
        FIELD("access", "%u read(s) remaining", (unsigned)st->access_remaining);

    /* ttl_remaining was captured at load; age it by the time since. */
    if (!st->has_ttl) {
        FIELD("ttl", "none");
    } else {
        uint64_t elapsed = now_ticks() - s->loaded_at;
        if (st->ttl_remaining > elapsed)
            FIELD("ttl", "%llu ms remaining",
                  (unsigned long long)ticks_to_ms(st->ttl_remaining - elapsed));
        else
            FIELD("ttl", "expired (awaiting)");
    }

    uint64_t age = (s->loaded_at > st->last_access) ? s->loaded_at - st->last_access : 0;
    FIELD("last access", "%llu ms before load", (unsigned long long)ticks_to_ms(age));
    FIELD("index", "%zu of %zu", s->selected + 1, s->count);

    if (!s->info_ok)
        return;

    const struct env_info *in = &s->info;
    cy++;
    LINE(HEAD_ATTR, "Environment status:");
    FIELD("slots", "used %zu|%zu", in->count, in->capacity);
    FIELD("bytes", "used %zu|%zu", in->used_bytes, in->max_bytes);
    FIELD("timer", "%s", in->timer_attached ? "attached" : "disabled");
    FIELD("ops", "set %llu, get %llu, peek %llu, unset %llu",
          (unsigned long long)in->n_set, (unsigned long long)in->n_get,
          (unsigned long long)in->n_peek, (unsigned long long)in->n_unset);
    FIELD("reclaimed", "acc %llu, ttl %llu, space %llu, exp %llu",
          (unsigned long long)in->n_reclaim_access, (unsigned long long)in->n_reclaim_time,
          (unsigned long long)in->n_reclaim_space, (unsigned long long)in->n_reclaim_explicit);
    FIELD("sweeps", "%llu", (unsigned long long)in->n_sweeps);
}

#undef FIELD
#undef LINE

static void draw(struct tui_view *v) {
    struct envtool_state *s = v->state;

    int w, h;
    tui_size(&w, &h);

    tui_clear(TUI_ATTR_DEFAULT);

    /* Title bar */
    if (s->info_ok && s->info.count > s->count)
        tui_text(0, 0, TUI_ATTR(TUI_FG_CYAN, 0),
                 "envtool  showing %zu of %zu variables (cache limit)",
                 s->count, s->info.count);
    else
        tui_text(0, 0, TUI_ATTR(TUI_FG_CYAN, 0),
                 "envtool  %zu environment variable%s",
                 s->count, s->count == 1 ? "" : "s");

    /* Split layout: Left pane for [ns:name], Right pane for metadata & value */
    int left_w = LEFT_W_DEFAULT;
    if (left_w > w - 20) left_w = w / 2;
    if (left_w < 16) left_w = 16;

    struct tui_rect list_frame   = { 0,      2, left_w,      h - 3 };
    struct tui_rect detail_frame = { left_w, 2, w - left_w, h - 3 };

    tui_box(list_frame,   TUI_ATTR(TUI_FG_WHITE, 0));
    tui_box(detail_frame, TUI_ATTR(TUI_FG_WHITE, 0));

    /* Left pane: list items */
    int rows = list_frame.h - 2;
    if (rows < 1) rows = 1;

    if (s->count == 0) {
        tui_text(2, 3, TUI_ATTR(TUI_FG_YELLOW, 0), "(env store is empty)");
    } else {
        if (s->selected < s->top)
            s->top = s->selected;
        if (s->selected >= s->top + (size_t)rows)
            s->top = s->selected - (size_t)rows + 1;

        for (int i = 0; i < rows; i++) {
            size_t idx = s->top + (size_t)i;
            if (idx >= s->count) break;
            const struct env_item *item = &s->items[idx];

            uint8_t attr = (idx == s->selected)
                         ? TUI_ATTR(TUI_FG_BLACK, TUI_FG_CYAN)
                         : TUI_ATTR_DEFAULT;

            char display_key[2 * ENV_NAME_MAX + 2];
            ksnprintf(display_key, sizeof(display_key), "%s:%s", item->ns, item->name);
            tui_text(2, 3 + i, attr, "%-28s", display_key);
        }
    }

    /* Right pane: details of the selected variable, then the device snapshot */
    if (s->selected >= s->count) {
        tui_text(left_w + 2, 3, TUI_ATTR(TUI_FG_YELLOW, 0), "(no variable selected)");
    } else {
        draw_detail(s, left_w + 2, 3, detail_frame.y + detail_frame.h - 1);
    }

    /* Status bar */
    tui_text(0, h - 1, TUI_ATTR(TUI_FG_YELLOW, 0),
             "up/down move   home/end jump   r reload   q quit");
}

static bool key(struct tui_view *v, enum kbd_event ev, char c) {
    struct envtool_state *s = v->state;

    if (ev == KBD_CHAR && c == 'q')
        return false;

    if (ev == KBD_CHAR && c == 'r') {
        load_env_items(s);
        return true;
    }

    if (s->count == 0)
        return true;

    if (ev == KBD_SPECIAL && c == KBD_KEY_UP) {
        if (s->selected > 0)
            s->selected--;
        return true;
    }
    if (ev == KBD_SPECIAL && c == KBD_KEY_DOWN) {
        if (s->selected + 1 < s->count)
            s->selected++;
        return true;
    }
    if (ev == KBD_SPECIAL && c == KBD_KEY_HOME) {
        s->selected = 0;
        return true;
    }
    if (ev == KBD_SPECIAL && c == KBD_KEY_END) {
        s->selected = s->count - 1;
        return true;
    }

    return true;
}


/*  inspect */


static int cmd_inspect(void) {
    if (bus_env() == NULL) {
        kprintf("envtool: no environment device registered\n");
        return 1;
    }

    static struct envtool_state s;
    load_env_items(&s);

    if (!tui_begin())
        return 1;

    struct tui_view v = {
        .draw  = draw,
        .key   = key,
        .state = &s,
    };
    tui_run(&v);

    tui_end();
    return CMD_OK;
}


/*  stress helpers */


/* Tiny xorshift. Seeded per fill() from the tick counter so successive runs differ. Not cryptographic; not meant to be. */
static uint32_t rng_state;

static uint32_t rng(void) {
    uint32_t x = rng_state ? rng_state : 0x9e3779b9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state = x;
    return x;
}

/* Parse a non-negative decimal. No strtol in lib/string.h, and we do not
 * want to drag in a libc. Returns -1 on empty or non-digit input. */
static int parse_uint(const char *s) {
    if (s == NULL || *s == '\0')
        return -1;
    int v = 0;
    for (; *s != '\0'; s++) {
        if (*s < '0' || *s > '9')
            return -1;
        if (v > (ENVTOOL_MAX_N / 10))
            return ENVTOOL_MAX_N + 1;   /* saturate; caller clamps */
        v = v * 10 + (*s - '0');
    }
    return v;
}


/*  fill */


int envtool_fill(int n) {
    const struct env_ops *env = bus_env();
    if (env == NULL) {
        kprintf("envtool: no environment device registered\n");
        return -1;
    }

    if (n <= 0)
        n = ENVTOOL_DEFAULT_N;
    if (n > ENVTOOL_MAX_N)
        n = ENVTOOL_MAX_N;

    rng_state = (uint32_t)now_ticks() | 1u;

    int ok = 0, fail = 0;

    for (int i = 0; i < n; i++) {
        char name[ENV_NAME_MAX + 1];
        char value[ENV_VALUE_MAX + 1];

        ksnprintf(name, sizeof name, "s%05u", (unsigned)i);

        /* Printable value of random length; long enough to exercise the
         * byte accounting without ever hitting ENV_VALUE_MAX. */
        unsigned vlen = 1u + (rng() % 16u);
        if (vlen > ENV_VALUE_MAX)
            vlen = ENV_VALUE_MAX;
        for (unsigned j = 0; j < vlen; j++)
            value[j] = (char)('a' + (rng() % 26u));
        value[vlen] = '\0';

        uint32_t mask = 0;
        uint64_t ttl  = 0;
        uint32_t budg = 0;
        uint8_t  imp;

        switch (rng() % 3u) {
        case 0:
            mask |= ENVTOOL_SHORT_TTL;
            ttl = ms_to_ticks_ceil(1000);
            break;
        case 1:
            mask |= ENVTOOL_LONG_TTL;
            ttl = ms_to_ticks_ceil(30000);
            break;
        default:
            mask |= ENVTOOL_NO_TTL;
            ttl = 0;
            break;
        }

        switch (rng() % 2u) {
        case 0:
            mask |= ENVTOOL_LOW_BUDGET;
            budg = 1u + (rng() % 2u);
            break;
        default:
            mask |= ENVTOOL_NO_BUDGET;
            budg = 0;
            break;
        }

        if (rng() & 1u) {
            mask |= ENVTOOL_HIGH_IMP;
            imp = ENV_IMPORTANCE_MAX;
        } else {
            mask |= ENVTOOL_LOW_IMP;
            imp = 0;
        }

        int rc = env->set(ENVTOOL_NS, name, value, budg, ttl, imp, mask);
        if (rc == ENV_OK)
            ok++;
        else
            fail++;
    }

    kprintf("envtool fill: %d ok, %d failed (ns=\"%s\")\n",
            ok, fail, ENVTOOL_NS);
    return ok;
}


/*  clear */


int envtool_clear(void) {
    const struct env_ops *env = bus_env();
    if (env == NULL) {
        kprintf("envtool: no environment device registered\n");
        return -1;
    }
    int rc = env->drop_namespace(ENVTOOL_NS);
    kprintf("envtool clear: rc=%d (ns=\"%s\")\n", rc, ENVTOOL_NS);
    return (rc == ENV_OK) ? 0 : rc;
}


/*  stats */


int envtool_stats(void) {
    const struct env_ops *env = bus_env();
    if (env == NULL) {
        kprintf("envtool: no environment device registered\n");
        return -1;
    }

    struct env_info in;
    if (env->info(&in) == ENV_OK) {
        kprintf("env: %zu/%zu slots, %zu/%zu bytes, "
                "sets=%llu gets=%llu peeks=%llu unsets=%llu\n",
                in.count, in.capacity, in.used_bytes, in.max_bytes,
                (unsigned long long)in.n_set,
                (unsigned long long)in.n_get,
                (unsigned long long)in.n_peek,
                (unsigned long long)in.n_unset);
        kprintf("     reclaims: acc=%llu ttl=%llu space=%llu expl=%llu, "
                "sweeps=%llu\n",
                (unsigned long long)in.n_reclaim_access,
                (unsigned long long)in.n_reclaim_time,
                (unsigned long long)in.n_reclaim_space,
                (unsigned long long)in.n_reclaim_explicit,
                (unsigned long long)in.n_sweeps);
        if (!in.timer_attached)
            kprintf("     warning: timer not attached — TTL and sweeps are inert\n");
    }

    static const struct {
        const char *label;
        uint32_t    bit;
    } cats[] = {
        { "short ttl",  ENVTOOL_SHORT_TTL  },
        { "long  ttl",   ENVTOOL_LONG_TTL   },
        { "no    ttl",     ENVTOOL_NO_TTL     },
        { "low   budget", ENVTOOL_LOW_BUDGET },
        { "no    budget",  ENVTOOL_NO_BUDGET  },
        { "low   imp",    ENVTOOL_LOW_IMP    },
        { "high  imp",   ENVTOOL_HIGH_IMP   },
    };

    for (size_t i = 0; i < sizeof cats / sizeof cats[0]; i++) {
        size_t n = 0;
        if (env->count_by_mask(cats[i].bit, &n) == ENV_OK)
            kprintf("  %-12s %zu\n", cats[i].label, n);
    }

    return 0;
}


/*  command dispatch */
static int cmd_envtool_fn(const struct cmd_args *a) {
    /* Spec: "<cmd:str> [arg:str...]"
     * argc >= 1 guaranteed by the dispatcher (cmd is required). */
    const char *sub = a->argv[0];

    if (strcmp(sub, "inspect") == 0)
        return cmd_inspect();

    if (strcmp(sub, "fill") == 0) {
        int n = ENVTOOL_DEFAULT_N;
        if (a->argc >= 2) {
            int parsed = parse_uint(a->argv[1]);
            if (parsed < 0) {
                kprintf("envtool fill: bad count '%s'\n", a->argv[1]);
                return CMD_USAGE;
            }
            n = parsed;
        }
        return (envtool_fill(n) >= 0) ? CMD_OK : 1;
    }

    if (strcmp(sub, "clear") == 0)
        return (envtool_clear() == 0) ? CMD_OK : 1;

    if (strcmp(sub, "stats") == 0)
        return (envtool_stats() == 0) ? CMD_OK : 1;

    kprintf("envtool: unknown subcommand '%s'\n", sub);
    return CMD_USAGE;
}

SYM_COMMAND(envtool, "<cmd:str> [arg:str...]",
            "Environment module inspection and stress tester",
            cmd_envtool_fn);

/* extension greeter */
static int envtool_ext_init(void) { return 0; }
SYM_EXTENSION(envtool, envtool_ext_init, EXT_PRIO_APPLET);
