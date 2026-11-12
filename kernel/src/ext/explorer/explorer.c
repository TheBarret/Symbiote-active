#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
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
};

static void path_join(const char *base, const char *name, char *out, size_t cap) {
    if (strcmp(base, "/") == 0)
        ksnprintf(out, cap, "/%s", name);
    else
        ksnprintf(out, cap, "%s/%s", base, name);
}

/* Harden: strip trailing slashes before finding the parent,
 * and handle the root case without manual index arithmetic. */
static void path_parent(char *path) {
    size_t len = strlen(path);

    /* Strip trailing slashes */
    while (len > 1 && path[len - 1] == '/') {
        path[--len] = '\0';
    }
    if (len <= 1) {
        return; // already root
    }

    /* Find the last slash */
    while (len > 0 && path[len - 1] != '/') {
        len--;
    }

    /* If we found a slash, remove it and everything after */
    if (len > 0) {
        path[len - 1] = '\0';
    }

    /* If we ended up with an empty string (e.g., "/foo"), make it "/" */
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

static void draw(struct tui_view *v) {
    struct explorer_state *s = v->state;

    int w, h;
    tui_size(&w, &h);

    tui_clear(TUI_ATTR_DEFAULT);

    /* Title: Use "%s" to prevent format-string injection via path */
    tui_text(0, 0, TUI_ATTR(TUI_FG_CYAN, 0), "explorer  %s", s->path);

    /* Frame around the list, rows 2..h-3 */
    struct tui_rect frame = { 0, 2, w, h - 3 };
    tui_box(frame, TUI_ATTR(TUI_FG_WHITE, 0));

    /* List, inside the frame */
    int rows = frame.h - 2;
    if (rows < 1)
        rows = 1;

    /* Keep the selected entry visible. */
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

        /* Use "%s" to prevent format-string injection via filename */
        tui_text(1, 3 + i, attr, "%-12s %s",
                 e->is_dir ? "<DIR>" : "",
                 e->name);
    }

    /* Status line */
    tui_text(0, h - 1, TUI_ATTR(TUI_FG_YELLOW, 0),
             "up/down move   enter descend   backspace up   q quit");
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
