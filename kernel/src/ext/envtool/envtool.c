/* envtool: read-only TUI inspector for the environment store. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <lib/string.h>
#include <core/console.h>
#include <core/keyboard.h>
#include <core/kprintf.h>
#include <core/cmd.h>
#include <core/ext.h>
#include <core/timer.h>
#include <core/bus.h>
#include <core/env.h>
#include <core/tui.h>

#define LEFT_W_DEFAULT   32
#define MAX_CACHED_ITEMS 64

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

/* helpers */

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

/* Load.
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

/* Detail pane. cx/cy/ymax are the macros' working variables; rows past ymax
 * are skipped so a short screen clips instead of overdrawing the frame. */

#define KEY_ATTR TUI_ATTR(TUI_FG_YELLOW, 0)
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
    FIELD("slots", "%zu used / %zu", in->count, in->capacity);
    FIELD("bytes", "%zu used / %zu", in->used_bytes, in->max_bytes);
    FIELD("timer", "%s", in->timer_attached ? "attached" : "none (no TTL, no sweep)");
    FIELD("ops", "set %llu  get %llu  peek %llu  unset %llu",
          (unsigned long long)in->n_set, (unsigned long long)in->n_get,
          (unsigned long long)in->n_peek, (unsigned long long)in->n_unset);
    FIELD("reclaimed", "acc %llu  ttl %llu  space %llu  expl %llu",
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

static int cmd_envtool_fn(const struct cmd_args *a) {
    (void)a;

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

SYM_COMMAND(envtool, "", "Inspect environment variable store", cmd_envtool_fn);

static int envtool_ext_init(void) { return 0; }
SYM_EXTENSION(envtool, envtool_ext_init, EXT_PRIO_APPLET);
