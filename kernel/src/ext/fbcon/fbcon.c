#include <stddef.h>
#include <stdint.h>
#include <core/version.h>
#include <core/arch/boot.h>
#include <core/dev/console.h>
#include <core/dev/env.h>
#include <core/kprintf.h>
#include <core/klog.h>
#include <core/bus.h>
#include <core/ext.h>
#include <core/mask.h>
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

    // pair up with console
    console_register(&fbcon_ops);           /* replays the boot log */

    // write framebuffer manifest
    const struct env_ops *env = bus_env();
    if (env != NULL) {
        int ret_value;
        char ns[64];
        ksnprintf(ns, sizeof(ns), "kernel.dev.console");
        uint32_t m = env_mask_build(1, ENV_TYPE_INT, ENV_FLAG_PINNED | ENV_FLAG_READONLY, 0);

        // Width/height vectors
        char buf_width[16], buf_height[16];
        ksnprintf(buf_width, sizeof(buf_width), "%zu", (size_t)fb->width);
        ksnprintf(buf_height, sizeof(buf_height), "%zu", (size_t)fb->height);
        ret_value = env->set(ns, "height", buf_height, 0, 0, 0, m);
        if (ret_value != ENV_OK) { klog_warning("fbcon_init(env->set): exit code = %u", ret_value); }
        ret_value = env->set(ns, "width", buf_width, 0, 0, 0, m);
        if (ret_value != ENV_OK) { klog_warning("fbcon_init(env->set): exit code = %u", ret_value); }

        // Column/row vectors
        size_t cols = 0, rows = 0;
        flanterm_get_dimensions(term, &cols, &rows);
        char buf_cols[16], buf_rows[16];
        ksnprintf(buf_cols, sizeof buf_cols, "%zu", cols);
        ksnprintf(buf_rows, sizeof buf_rows, "%zu", rows);
        ret_value = env->set(ns, "cols", buf_cols, 0, 0, 0, m);
        if (ret_value != ENV_OK) { klog_warning("fbcon_init(env->set): exit code = %u", ret_value); }
        ret_value = env->set(ns, "rows", buf_rows, 0, 0, 0, m);
        if (ret_value != ENV_OK) { klog_warning("fbcon_init(env->set): exit code = %u", ret_value); }
    }

    return 0;
}

SYM_EXTENSION(fbcon, fbcon_init, EXT_PRIO_CONSOLE);
