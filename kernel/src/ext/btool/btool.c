/* btool - read only bus viewer  */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <core/bus.h>
#include <core/cmd.h>
#include <core/ext.h>
#include <core/kprintf.h>
#include <core/tui.h>
#include <core/version.h>
#include <lib/string.h>


/* state */


#define LEFT_W_DEFAULT  28

struct btool_state {
    size_t selected;    /* index into the bus table */
    size_t top;         /* first visible row */
};

/* Class name lookup. */
static const char *class_name(enum bus_class c) {
    switch (c) {
    case BUS_CLASS_NONE:    return "none";      // empty (interior)
    case BUS_CLASS_CONSOLE: return "console";   // console (default)
    case BUS_CLASS_TIMER:   return "timer";     // interrupts (default)
    case BUS_CLASS_VFS:     return "vfs";       // ramfs (default)
    case BUS_CLASS_LOG:     return "log";       // TODO
    case BUS_CLASS_INPUT:   return "input";     // TODO
    case BUS_CLASS_USER:    return "user";      // TODO
    case BUS_CLASS_ENV:     return "env";       // environment variable storage
    default:                return "UNDEFINED"; // state the obvious
    }
}


/* draw */


static void draw(struct tui_view *v) {
    struct btool_state *s = v->state;

    int w, h;
    tui_size(&w, &h);

    tui_clear(TUI_ATTR_DEFAULT);

    size_t n = bus_count();

    /* Title bar */
    tui_text(0, 0, TUI_ATTR(TUI_FG_CYAN, 0),
             "btool   %zu device%s registered",
             n, n == 1 ? "" : "s");

    /* Right pane starts at column LEFT_W. Frame is row 2 .. h-2. */
    int left_w  = LEFT_W_DEFAULT;
    if (left_w > w - 20) left_w = w / 2;   /* degenerate narrow terminal */
    if (left_w < 12)     left_w = 12;

    struct tui_rect list_frame  = { 0,     2, left_w,        h - 3 };
    struct tui_rect detail_frame = { left_w, 2, w - left_w, h - 3 };

    tui_box(list_frame,   TUI_ATTR(TUI_FG_WHITE, 0));
    tui_box(detail_frame, TUI_ATTR(TUI_FG_WHITE, 0));

    /* Left pane: one row per device, scrolling to keep selection visible */
    int rows = list_frame.h - 2;
    if (rows < 1) rows = 1;

    if (n == 0) {
        tui_text(2, 3, TUI_ATTR(TUI_FG_YELLOW, 0),
                 "(bus is empty)");
    } else {
        if (s->selected < s->top)
            s->top = s->selected;
        if (s->selected >= s->top + (size_t)rows)
            s->top = s->selected - (size_t)rows + 1;

        for (int i = 0; i < rows; i++) {
            size_t idx = s->top + (size_t)i;
            if (idx >= n) break;
            const struct bus_device *d = bus_at(idx);
            if (d == NULL || d->name == NULL) continue;

            uint8_t attr = (idx == s->selected)
                         ? TUI_ATTR(TUI_FG_BLACK, TUI_FG_CYAN)
                         : TUI_ATTR_DEFAULT;

            tui_text(2, 3 + i, attr, "%-20s", d->name);
        }
    }

    /* Right pane: details of the selected device */
    if (s->selected >= n) {
        tui_text(left_w + 2, 3, TUI_ATTR(TUI_FG_YELLOW, 0),
                 "(no device selected)");
    } else {
        const struct bus_device *d = bus_at(s->selected);
        if (d == NULL) {
            tui_text(left_w + 2, 3, TUI_ATTR(TUI_FG_RED, 0),
                     "(invalid index)");
        } else {
            int cx = left_w + 2;
            int cy = 3;

            tui_text(cx, cy++, TUI_ATTR(TUI_FG_YELLOW, 0), "name:");
            tui_text(cx + 8, cy - 1, TUI_ATTR_DEFAULT, "%s",
                     d->name ? d->name : "(null)");

            tui_text(cx, cy++, TUI_ATTR(TUI_FG_YELLOW, 0), "class:");
            tui_text(cx + 8, cy - 1, TUI_ATTR_DEFAULT, "%u (%s)",
                     (unsigned)d->class,
                     class_name(d->class));

            tui_text(cx, cy++, TUI_ATTR(TUI_FG_YELLOW, 0), "version:");
            tui_text(cx + 10, cy - 1, TUI_ATTR_DEFAULT, "%u",
                     (unsigned)d->version);

            tui_text(cx, cy++, TUI_ATTR(TUI_FG_YELLOW, 0), "ops:");
            tui_text(cx + 6, cy - 1, TUI_ATTR_DEFAULT, "0x%llx",
                     (unsigned long long)(uintptr_t)d->ops);

            tui_text(cx, cy++, TUI_ATTR(TUI_FG_YELLOW, 0), "private:");
            tui_text(cx + 10, cy - 1, TUI_ATTR_DEFAULT, "0x%llx",
                     (unsigned long long)(uintptr_t)d->private_data);

            cy++;
            tui_text(cx, cy++, TUI_ATTR(TUI_FG_WHITE, 0),
                     "index: %zu of %zu", s->selected + 1, n); // added: +1 for cosmetic fix
        }
    }

    /* Status bar */
    tui_text(0, h - 1, TUI_ATTR(TUI_FG_YELLOW, 0),
             "up/down move   home/end jump   q quit");
}


/* key */


static bool key(struct tui_view *v, enum kbd_event ev, char c) {
    struct btool_state *s = v->state;
    size_t n = bus_count();

    if (ev == KBD_CHAR && c == 'q')
        return false;

    if (n == 0)
        return true;

    if (ev == KBD_SPECIAL && c == KBD_KEY_UP) {
        if (s->selected > 0)
            s->selected--;
        return true;
    }
    if (ev == KBD_SPECIAL && c == KBD_KEY_DOWN) {
        if (s->selected + 1 < n)
            s->selected++;
        return true;
    }
    if (ev == KBD_SPECIAL && c == KBD_KEY_HOME) {
        s->selected = 0;
        return true;
    }
    if (ev == KBD_SPECIAL && c == KBD_KEY_END) {
        s->selected = n - 1;
        return true;
    }

    return true;
}


/* entry point */


static int cmd_btool_fn(const struct cmd_args *a) {
    (void)a;   /* no arguments: the bus is flat, there is nothing to navigate to */

    static struct btool_state s;
    s.selected = 0;
    s.top = 0;

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

SYM_COMMAND(btool, "", "Bus Tool (Inspect bus devices) ", cmd_btool_fn);

static int btool_ext_init(void) { return 0; }
SYM_EXTENSION(btool, btool_ext_init, EXT_PRIO_APPLET);
