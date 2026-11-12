#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <lib/mem.h>
#include <lib/string.h>
#include <core/dev/console.h>
#include <core/dev/keyboard.h>
#include <core/dev/timer.h>
#include <core/dev/vfs.h>
#include <core/kprintf.h>
#include <core/cmd.h>
#include <core/ext.h>
#include <core/tui.h>

#define PATH_MAX 128
#define ENTRY_MAX 128
#define PREVIEW_MAX 2048

struct entry {
    char name[VFS_NAME_MAX + 1];
    bool is_dir;
    uint64_t size;
};

struct explorer_state {
    char path[PATH_MAX];
    struct entry entries[ENTRY_MAX];
    size_t count;
    size_t selected;
    size_t top;         /* scroll offset */

    /* Preview cache */
    char   preview[PREVIEW_MAX];
    size_t preview_len;
    bool   preview_is_dir;
    size_t preview_sel; /* which entry index the cache belongs to */
};

static void path_join(const char *base, const char *name, char *out, size_t cap) {
    if (strcmp(base, "/") == 0)
        ksnprintf(out, cap, "/%s", name);
    else
        ksnprintf(out, cap, "%s/%s", base, name);
}

static void path_parent(char *path) {
    size_t len = strlen(path);

    while (len > 1 && path[len - 1] == '/') {
        path[--len] = '\0';
    }
    if (len <= 1) {
        return;
    }

    while (len > 0 && path[len - 1] != '/') {
        len--;
    }

    if (len > 0) {
        path[len - 1] = '\0';
    }

    if (path[0] == '\0') {
        path[0] = '/';
        path[1] = '\0';
    }
}

static void load_dir(struct explorer_state *s, const char *path) {
    strncpy(s->path, path, PATH_MAX - 1);
    s->path[PATH_MAX - 1] = '\0';
    s->count = 0;
    s->selected = 0;
    s->top = 0;

    /* Force preview invalidation on directory change */
    s->preview_sel = (size_t)-1;

    size_t iter = 0;
    char name[VFS_NAME_MAX + 1];
    while (s->count < ENTRY_MAX && vfs_readdir(path, &iter, name)) {
        struct entry *e = &s->entries[s->count];
        strncpy(e->name, name, VFS_NAME_MAX);
        e->name[VFS_NAME_MAX] = '\0';

        char full[PATH_MAX];
        path_join(path, name, full, sizeof full);
        struct vfs_stat st;
        if (vfs_stat(full, &st) == 0) {
            e->is_dir = (st.type == VFS_DIR);
            e->size = st.size;
        } else {
            e->is_dir = false;
            e->size = 0;
        }
        s->count++;
    }
}

static void load_preview(struct explorer_state *s) {
    s->preview_len = 0;
    s->preview[0] = '\0';
    s->preview_is_dir = false;
    s->preview_sel = s->selected; // Mark as up-to-date immediately

    if (s->count == 0 || s->selected >= s->count) return;

    struct entry *e = &s->entries[s->selected];
    if (e->is_dir) {
        s->preview_is_dir = true;
        return;
    }

    char full[PATH_MAX];
    path_join(s->path, e->name, full, sizeof full);

    struct vfs_file *f = vfs_open(full, VFS_O_RDONLY);
    if (!f) return;

    /* Cap at PREVIEW_MAX - 1 so we always NUL-terminate */
    long n = vfs_read(f, s->preview, PREVIEW_MAX - 1);
    vfs_close(f);

    if (n > 0) {
        s->preview_len = (size_t)n;
        s->preview[n] = '\0';
    }
}

static void draw(struct tui_view *v) {
    struct explorer_state *s = v->state;

    int w, h;
    tui_size(&w, &h);

    tui_clear(TUI_ATTR_DEFAULT);

    /* Title */
    tui_text(0, 0, TUI_ATTR(TUI_FG_CYAN, 0), "explorer  %s", s->path);

    /* Status line */
    tui_text(0, h - 1, TUI_ATTR(TUI_FG_YELLOW, 0),
             "up/down move   enter descend   backspace up   q quit");

    /* Panel geometry: split 50/50 */
    int list_w = w / 2;
    int prev_w = w - list_w;

    struct tui_rect list_frame = { 0, 2, list_w, h - 3 };
    struct tui_rect prev_frame = { list_w, 2, prev_w, h - 3 };

    tui_box(list_frame, TUI_ATTR(TUI_FG_WHITE, 0));
    tui_box(prev_frame, TUI_ATTR(TUI_FG_WHITE, 0));

    /* --- LEFT PANE (File List) --- */
    int rows = list_frame.h - 2;
    if (rows < 1) rows = 1;

    /* Ensure selection stays visible */
    if (s->selected < s->top)
        s->top = s->selected;
    if (s->selected >= s->top + (size_t)rows)
        s->top = s->selected - (size_t)rows + 1;

    for (int i = 0; i < rows && s->top + (size_t)i < s->count; i++) {
        size_t idx = s->top + (size_t)i;
        struct entry *e = &s->entries[idx];
        uint32_t attr = (idx == s->selected)
                      ? TUI_ATTR(TUI_FG_BLACK, TUI_FG_CYAN)
                      : TUI_ATTR_DEFAULT;

        /* Pad with spaces to fill the highlight bar if selected */
        tui_text(1, 3 + i, attr, "%-12s %-*s",
                 e->is_dir ? "<DIR>" : "",
                 list_w - 16, /* remaining width approx */
                 e->name);
    }

    /* --- RIGHT PANE (Preview) --- */
    if (s->preview_sel != s->selected) {
        load_preview(s);
    }

    if (s->preview_is_dir) {
        tui_text(list_w + 1, 4, TUI_ATTR(TUI_FG_WHITE, 0), "<directory>");
    } else if (s->preview_len == 0) {
        tui_text(list_w + 1, 4, TUI_ATTR(TUI_FG_WHITE, 0), "(empty or unreadable)");
    } else {
        int prows = prev_frame.h - 2;
        if (prows < 1) prows = 1;

        int col = list_w + 1;
        int row = 4;
        size_t pos = 0;

        for (int i = 0; i < prows && pos < s->preview_len; i++) {
            size_t line_start = pos;
            while (pos < s->preview_len && s->preview[pos] != '\n') pos++;
            size_t line_len = pos - line_start;
            if (pos < s->preview_len) pos++; /* skip '\n' */

            /* Clip to fit inside the box margins */
            if (line_len > (size_t)(prev_w - 2))
                line_len = (size_t)(prev_w - 2);

            char linebuf[256];
            if (line_len >= sizeof(linebuf)) line_len = sizeof(linebuf) - 1;
            memcpy(linebuf, s->preview + line_start, line_len);
            linebuf[line_len] = '\0';

            tui_text(col, row + i, TUI_ATTR(TUI_FG_WHITE, 0), "%s", linebuf);
        }

        /* Truncation indicator */
        if (s->preview_len == PREVIEW_MAX - 1 && prows > 0) {
            tui_text(col, row + prows - 1, TUI_ATTR(TUI_FG_YELLOW, 0), "… truncated");
        }
    }
}

static bool key(struct tui_view *v, enum kbd_event ev, char c) {
    struct explorer_state *s = v->state;

    if (ev == KBD_CHAR && c == 'q')
        return false;

    if (ev == KBD_CHAR && c == '\n') {
        if (s->count == 0 || s->selected >= s->count)
            return true;
        struct entry *e = &s->entries[s->selected];
        if (e->is_dir) {
            char next[PATH_MAX];
            path_join(s->path, e->name, next, sizeof next);
            load_dir(s, next);
        }
        return true;
    }

    if (ev == KBD_CHAR && c == '\b') {
        if (strcmp(s->path, "/") != 0) {
            path_parent(s->path);
            load_dir(s, s->path);
        }
        return true;
    }

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

    return true;
}

static int cmd_explorer_fn(const struct cmd_args *a) {
    const char *start = (a->argc >= 1) ? a->argv[0] : "/";

    static struct explorer_state s;
    load_dir(&s, start);

    if (!tui_begin())
        return 1;

    struct tui_view v = {
        .draw = draw,
        .key = key,
        .state = &s,
    };
    tui_run(&v);

    tui_end();
    return CMD_OK;
}

SYM_COMMAND(explorer, "[path:str]", "Browse the VFS", cmd_explorer_fn);

static int explorer_ext_init(void) { return 0; }
SYM_EXTENSION(explorer, explorer_ext_init, EXT_PRIO_APPLET);
