#include <stddef.h>
#include <stdint.h>
#include <core/version.h>
#include <core/arch/boot.h>
#include <core/dev/console.h>
#include <core/ext.h>
#include <core/kprintf.h>
#include <flanterm.h>
#include <flanterm_backends/fb.h>

/* Framebuffer console extension.
 * flanterm draws text (with a built-in font, scrolling and ANSI colours) onto
 * the framebuffer Limine gave us. Without this extension the image is
 * headless and only the serial console exists. */

static struct flanterm_context *term;

/* flanterm behaves like a real terminal: '\n' only moves DOWN a line. Our
 * kprintf uses plain '\n' for "newline", so convert it to CR+LF here, just as
 * the serial console does. */
static void fbcon_write(const char *buf, size_t len) {
    size_t start = 0;
    for (size_t i = 0; i < len; i++) {
        if (buf[i] == '\n') {
            flanterm_write(term, buf + start, i - start);
            flanterm_write(term, "\r\n", 2);
            start = i + 1;
        }
    }
    flanterm_write(term, buf + start, len - start);
}

static const struct console_ops fbcon_ops = { "fbcon", fbcon_write };

/* Symbiote theme from config.h*/
static uint32_t theme_bg = SYM_FBCON_BG;
static uint32_t theme_fg = SYM_FBCON_FG;

static int fbcon_init(void) {
    struct limine_framebuffer *fb = boot_framebuffer();
    if (fb == NULL)
        return 1;           /* no framebuffer: stay headless */
    if (fb->memory_model != LIMINE_FRAMEBUFFER_RGB || fb->bpp != 32)
        return 2;           /* only 32-bit RGB is supported */

    term = flanterm_fb_init(
        NULL, NULL,                         /* no allocator yet: flanterm uses its own pool */
        fb->address, fb->width, fb->height, fb->pitch,
        fb->red_mask_size, fb->red_mask_shift,
        fb->green_mask_size, fb->green_mask_shift,
        fb->blue_mask_size, fb->blue_mask_shift,
        NULL,                               /* no canvas */
        NULL, NULL,                         /* default ANSI colours */
        &theme_bg, &theme_fg,               /* our default background / foreground */
        NULL, NULL,                         /* default bright variants */
        NULL, 0, 0, 1,                      /* built-in font */
        0, 0,                               /* auto-scale font to resolution */
        0,                                  /* margin */
        FLANTERM_FB_ROTATE_0,
        true);                              /* autoflush */
    if (term == NULL)
        return 3;

    console_register(&fbcon_ops);           /* replays the boot log */
    return 0;
}

SYM_EXTENSION(fbcon, fbcon_init, EXT_PRIO_CONSOLE);
