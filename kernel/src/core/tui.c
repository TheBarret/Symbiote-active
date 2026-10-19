#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <lib/mem.h>
#include <lib/string.h>
#include <core/arch/heap.h>
#include <core/dev/keyboard.h>
#include <core/dev/console.h>
#include <core/dev/env.h>
#include <core/kprintf.h>
#include <core/tui.h>
#include <core/mask.h>

#define TUI_FLUSH_BUF 4096
#define TUI_FLUSH_DRAIN_AT (TUI_FLUSH_BUF - 64) /* Generous safety headroom */

struct cell {
    unsigned char ch;
    uint8_t attr;
};

static struct cell *front = NULL;
static struct cell *back = NULL;
static int tui_w = 0;
static int tui_h = 0;

/* Default dimensions */
// Moved to: core/config.h
//#define TUI_W 100
//#define TUI_H 25

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

/* resolution getter */
bool tui_fetch_resolution(const char *ns, const char *name, int *val_out) {
    if (ns == NULL || name == NULL || val_out == NULL)
        return false;

    const struct env_ops *env = bus_env();
    if (env == NULL)
        return false;

    /* inspect metadata */
    struct env_stat st;
    if (env->stat(ns, name, &st) != ENV_OK)
        return false;

    /* verify */
    if (env_mask_type(st.mask) != ENV_TYPE_INT) {
        // Optional: log warning that type contract was violated
        return false;
    }

    /* retrieve */
    char buf[32];
    size_t len = 0;
    if (env->peek(ns, name, buf, sizeof(buf), &len) != ENV_OK)
        return false;

    /* 3. Parse safely */
    uint64_t parsed = strtoull(buf, NULL, 10);
    if (parsed == 0 || parsed > INT32_MAX)
        return false;

    *val_out = (int)parsed;
    return true;
}

bool tui_begin(void) {
    /* Prevent double-allocation leaks if already active */
    if (front || back) { tui_end(); }

    /* get dimension from env store or fallback on defaults */
    tui_w = 100; tui_h = 25;                     /* fallback = 100x25 */
    int c = 0, r = 0;
    if (tui_fetch_resolution("kernel.dev.console", "cols", &c)) tui_w = c;
    if (tui_fetch_resolution("kernel.dev.console", "rows", &r)) tui_h = r;
    if (tui_w < 80) tui_w = 80; // validate both
    if (tui_h < 24) tui_h = 24;

    /* continue TUI init */
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

void tui_put(int x, int y,unsigned char ch, uint8_t attr) {
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

void tui_fill(struct tui_rect r,unsigned char ch, uint8_t attr) {
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

    tui_put(x0, y0, TUI_TL, attr);
    tui_put(x1, y0, TUI_TR, attr);
    tui_put(x0, y1, TUI_BL, attr);
    tui_put(x1, y1, TUI_BR, attr);

    for (int x = x0 + 1; x < x1; x++) {
        tui_put(x, y0, TUI_H, attr);
        tui_put(x, y1, TUI_H, attr);
    }
    for (int y = y0 + 1; y < y1; y++) {
        tui_put(x0, y, TUI_V, attr);
        tui_put(x1, y, TUI_V, attr);
    }
}

static inline void buf_append(char *out, size_t *len, size_t max, const char *src, size_t srclen) {
    if (*len + srclen <= max) {
        memcpy(out + *len, src, srclen);
        *len += srclen;
    }
}

/* CP437 characters to its UTF-8 encoding, or NULL for plain ASCII.
 * Every glyph below is exactly 3 bytes in UTF-8. */
static const char *cp437_utf8(unsigned char c) {
    switch (c) {
    /* single lines */
    case 0xC4: return "\xE2\x94\x80";   /* ─ */
    case 0xB3: return "\xE2\x94\x82";   /* │ */
    case 0xDA: return "\xE2\x94\x8C";   /* ┌ */
    case 0xBF: return "\xE2\x94\x90";   /* ┐ */
    case 0xC0: return "\xE2\x94\x94";   /* └ */
    case 0xD9: return "\xE2\x94\x98";   /* ┘ */
    case 0xC3: return "\xE2\x94\x9C";   /* ├ */
    case 0xB4: return "\xE2\x94\xA4";   /* ┤ */
    case 0xC2: return "\xE2\x94\xAC";   /* ┬ */
    case 0xC1: return "\xE2\x94\xB4";   /* ┴ */
    case 0xC5: return "\xE2\x94\xBC";   /* ┼ */
    /* double lines */
    case 0xCD: return "\xE2\x95\x90";   /* ═ */
    case 0xBA: return "\xE2\x95\x91";   /* ║ */
    case 0xC9: return "\xE2\x95\x94";   /* ╔ */
    case 0xBB: return "\xE2\x95\x97";   /* ╗ */
    case 0xC8: return "\xE2\x95\x9A";   /* ╚ */
    case 0xBC: return "\xE2\x95\x9D";   /* ╝ */
    /* blocks and shades */
    case 0xDB: return "\xE2\x96\x88";   /* █ */
    case 0xDF: return "\xE2\x96\x80";   /* ▀ */
    case 0xDC: return "\xE2\x96\x84";   /* ▄ */
    case 0xB0: return "\xE2\x96\x91";   /* ░ */
    case 0xB1: return "\xE2\x96\x92";   /* ▒ */
    case 0xB2: return "\xE2\x96\x93";   /* ▓ */
    default:   return NULL;
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

            /* Writing the last cell advances the cursor off-screen and scrolls. */
            if (x == tui_w - 1 && y == tui_h - 1) {
                *f = *b;
                continue;
            }

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

            //if (len < sizeof out) {
            //    out[len++] = b->ch;
            //}

            /* trap special chars */
            const char *ch_cp437 = cp437_utf8(b->ch);
            if (ch_cp437)
                buf_append(out, &len, sizeof out, ch_cp437, 3);
            else if (len < sizeof out)
                out[len++] = (char)b->ch;

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
