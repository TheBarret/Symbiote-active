#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <core/console.h>
#include <core/heap.h>
#include <core/keyboard.h>
#include <core/kprintf.h>
#include <core/tui.h>
#include <lib/mem.h>
#include <lib/string.h>

struct cell {
    char    ch;
    uint8_t attr;
};

static struct cell *front = NULL;
static struct cell *back = NULL;
static int tui_w = 0;
static int tui_h = 0;

/* Default dimensions */
#define TUI_W 80
#define TUI_H 25

static const char tui_enter[] = "\x1b[0m\x1b[2J\x1b[H\x1b[?25l";
static const char tui_leave[] = "\x1b[0m\x1b[2J\x1b[H\x1b[?25h";

static const char *const sgr_fg[8] = {
    "\x1b[30m", "\x1b[31m", "\x1b[32m", "\x1b[33m",
    "\x1b[34m", "\x1b[35m", "\x1b[36m", "\x1b[37m",
};
static const char *const sgr_bg[8] = {
    "\x1b[40m", "\x1b[41m", "\x1b[42m", "\x1b[43m",
    "\x1b[44m", "\x1b[45m", "\x1b[46m", "\x1b[47m",
};

bool tui_begin(void) {
    /* Prevent double-allocation leaks if already active */
    if (front || back) {
        tui_end();
    }

    tui_w = TUI_W;
    tui_h = TUI_H;

    size_t cells = (size_t)tui_w * (size_t)tui_h;
    front = kmalloc(cells * sizeof *front);
    back  = kmalloc(cells * sizeof *back);
    if (!front || !back) {
        kfree(front);
        kfree(back);
        front = back = NULL;
        return false;
    }

    memset(front, 0, cells * sizeof *front);
    memset(back,  0, cells * sizeof *back);

    console_write(tui_enter, sizeof tui_enter - 1);
    return true;
}

void tui_end(void) {
    if (!front && !back)
        return;

    console_write(tui_leave, sizeof tui_leave - 1);
    kfree(front);
    kfree(back);
    front = back = NULL;
}

void tui_size(int *w, int *h) {
    if (w) *w = tui_w;
    if (h) *h = tui_h;
}

static inline bool in_bounds(int x, int y) {
    return x >= 0 && x < tui_w && y >= 0 && y < tui_h;
}

static inline struct cell *cell_at(struct cell *grid, int x, int y) {
    return &grid[(size_t)y * (size_t)tui_w + (size_t)x];
}

void tui_clear(uint8_t attr) {
    if (!back)
        return;
    size_t cells = (size_t)tui_w * (size_t)tui_h;
    for (size_t i = 0; i < cells; i++) {
        back[i].ch = ' ';
        back[i].attr = attr;
    }
}

void tui_put(int x, int y, char ch, uint8_t attr) {
    if (!back || !in_bounds(x, y))
        return;
    struct cell *c = cell_at(back, x, y);
    c->ch = ch;
    c->attr = attr;
}

void tui_text(int x, int y, uint8_t attr, const char *fmt, ...) {
    if (!back || y < 0 || y >= tui_h)
        return;
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = kvsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if (n > (int)sizeof buf - 1)
        n = (int)sizeof buf - 1;

    for (int i = 0; i < n; i++) {
        int xx = x + i;
        if (xx >= tui_w)
            break;
        if (xx < 0)
            continue;
        tui_put(xx, y, buf[i], attr);
    }
}

void tui_fill(struct tui_rect r, char ch, uint8_t attr) {
    if (!back)
        return;
    for (int y = r.y; y < r.y + r.h; y++) {
        for (int x = r.x; x < r.x + r.w; x++) {
            tui_put(x, y, ch, attr);
        }
    }
}

void tui_box(struct tui_rect r, uint8_t attr) {
    if (r.w < 2 || r.h < 2)
        return;
    int x0 = r.x, y0 = r.y;
    int x1 = r.x + r.w - 1, y1 = r.y + r.h - 1;

    tui_put(x0, y0, '+', attr);
    tui_put(x1, y0, '+', attr);
    tui_put(x0, y1, '+', attr);
    tui_put(x1, y1, '+', attr);

    for (int x = x0 + 1; x < x1; x++) {
        tui_put(x, y0, '-', attr);
        tui_put(x, y1, '-', attr);
    }
    for (int y = y0 + 1; y < y1; y++) {
        tui_put(x0, y, '|', attr);
        tui_put(x1, y, '|', attr);
    }
}

#define TUI_FLUSH_BUF 4096
#define TUI_FLUSH_DRAIN_AT (TUI_FLUSH_BUF - 64) /* Generous safety headroom */

static inline void buf_append(char *out, size_t *len, size_t max, const char *src, size_t srclen) {
    if (*len + srclen <= max) {
        memcpy(out + *len, src, srclen);
        *len += srclen;
    }
}

void tui_flush(void) {
    if (!front || !back)
        return;

    char out[TUI_FLUSH_BUF];
    size_t len = 0;

    int last_x = -1, last_y = -1;
    uint8_t last_attr = 0xFF;

    for (int y = 0; y < tui_h; y++) {
        for (int x = 0; x < tui_w; x++) {
            struct cell *f = cell_at(front, x, y);
            struct cell *b = cell_at(back, x, y);
            if (f->ch == b->ch && f->attr == b->attr)
                continue;

            /* Drain if buffer approaches capacity */
            if (len >= TUI_FLUSH_DRAIN_AT) {
                console_write(out, len);
                len = 0;
                last_x = -1;
                last_y = -1;
                last_attr = 0xFF;
            }

            /* Move cursor if needed */
            if (x != last_x || y != last_y) {
                char seq[16];
                int n = ksnprintf(seq, sizeof seq, "\x1b[%d;%dH", y + 1, x + 1);
                if (n > 0) {
                    buf_append(out, &len, sizeof out, seq, (size_t)n);
                }
                last_x = x;
                last_y = y;
            }

            /* Set attributes if changed */
            if (b->attr != last_attr) {
                uint8_t fg = b->attr & 0x0F;
                uint8_t bg = (b->attr >> 4) & 0x0F;

                const char *reset = "\x1b[0m";
                buf_append(out, &len, sizeof out, reset, 4);

                if (b->attr != 0) {
                    if (fg < 8) {
                        const char *s = sgr_fg[fg];
                        buf_append(out, &len, sizeof out, s, strlen(s));
                    }
                    if (bg != 0 && bg < 8) {
                        const char *s = sgr_bg[bg];
                        buf_append(out, &len, sizeof out, s, strlen(s));
                    }
                }
                last_attr = b->attr;
            }

            if (len < sizeof out) {
                out[len++] = b->ch;
            }

            last_x = x + 1;
            last_y = y;

            *f = *b;
        }
    }

    if (len > 0) {
        console_write(out, len);
    }
}

void tui_run(struct tui_view *v) {
    if (!v)
        return;

    if (v->draw)
        v->draw(v);
    tui_flush();

    for (;;) {
        char c = 0;
        enum kbd_event ev = kbd_wait(&c);

        if (v->key && !v->key(v, ev, c))
            return;

        if (v->draw)
            v->draw(v);
        tui_flush();
    }
}

bool tui_run_view(struct tui_view *v) {
    if (!tui_begin())
        return false;
    tui_run(v);
    tui_end();
    return true;
}
