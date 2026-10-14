#ifndef CORE_TUI_H
#define CORE_TUI_H

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <core/keyboard.h>

/* Micro Terminal UI core
 *
 * Two cell grids (front and back), a rect-based drawing API,
 * and a flush that emits only the cells that changed.
 * Extensions draw into the back grid;
 * tui_flush diffs it against the front grid and writes the diff to the console in one call.
 * Nothing here knows about windows, clipping, or widgets; a view is a draw callback and a key callback.
 *
 * Screen ownership invariant
 * --------------------------
 * tui_begin and tui_end bracket a full-screen TUI session.
 *
 *   Before tui_begin: the console owns the screen. Default SGR,
 *     cursor visible, cursor position unspecified.
 *   Between them:     the TUI owns the screen. The cursor is hidden,
 *     the screen has been cleared, and every visible cell is
 *     whatever the last tui_flush emitted.
 *   After tui_end:    the console owns the screen again. Default SGR,
 *     cursor visible, cursor at the top-left cell (0,0).
 *
 * Callers that don't need manual control over the session should use
 * tui_run_view, which guarantees the pair even if the view's callbacks
 * return early.
 */

struct tui_rect {
    int x, y, w, h;
};

/* Attribute byte: low nibble = foreground, high nibble = background.
 * 0 means "default", which the flush leaves to the terminal.
 *
 * Patch note (2026-10): background is honored by tui_flush now. It was
 * accepted by TUI_ATTR but silently dropped by the SGR emitter, so a
 * caller who asked for a background got the default one with no warning.
 * Both nibbles are emitted; a zero background nibble emits nothing for
 * the background, so TUI_ATTR_DEFAULT still means "leave it alone". */
#define TUI_FG_BLACK   0x0
#define TUI_FG_RED     0x1
#define TUI_FG_GREEN   0x2
#define TUI_FG_YELLOW  0x3
#define TUI_FG_BLUE    0x4
#define TUI_FG_MAGENTA 0x5
#define TUI_FG_CYAN    0x6
#define TUI_FG_WHITE   0x7
#define TUI_ATTR(fg, bg) ((uint8_t)((fg) | ((bg) << 4)))
#define TUI_ATTR_DEFAULT 0

/* Start / stop. tui_begin allocates the grids, clears the screen, resets SGR, and hides the cursor.
 * tui_end resets SGR, clears the screen, homes the cursor, shows it,
 * and frees the grids. Both are safe to call when the TUI is not up. */
bool tui_begin(void);
void tui_end(void);

/* Screen size, in cells. Fixed at 80x25 for now;
 * the ops struct has no size query. A later get_size hook would replace this. */
void tui_size(int *w, int *h);

/* Drawing, all of these write into the back grid,
 * nothing is emitted to,
 * the console until tui_flush,
 * out-of-range coordinates are ignored. */
void tui_clear(uint8_t attr);
void tui_put(int x, int y, char ch, uint8_t attr);
void tui_text(int x, int y, uint8_t attr, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));
void tui_fill(struct tui_rect r, char ch, uint8_t attr);
void tui_box(struct tui_rect r, uint8_t attr);

/* Emit the diff between back and front to the console, then copy back to front. Call once per frame, after drawing.
 *
 * Drained to console_write in chunks instead of being accumulated into a fixed 8 KiB buffer.
 * When the old buffer filled mid-frame,
 * the remaining cells were silently dropped and the front grid was left out of sync with the back grid,
 * so the dropped cells never repainted. The chunked drain removes the class of bug rather than just widening the buffer. */
void tui_flush(void);

/* A view: draw into the grid, handle a key. key returns false to quit. */
struct tui_view {
    void (*draw)(struct tui_view *v);
    bool (*key)(struct tui_view *v, enum kbd_event ev, char c);
    void *state;
};

/* Run a view until its key callback returns false. The caller is responsible for pairing tui_begin / tui_end around this. */
void tui_run(struct tui_view *v);

/* Run a view, bracketing it with tui_begin / tui_end.
 * Returns false if the grids could not be allocated.
 *
 * Patch note (2026-10): added so a caller cannot forget the pairing.
 * Before this, a view that returned early, or a draw callback that panicked,
 * left the cursor hidden and the screen stuck in TUI mode.
 * Prefer this over calling tui_run directly. */
bool tui_run_view(struct tui_view *v);

#endif
