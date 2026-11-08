#include <stddef.h>
#include <stdint.h>
#include <core/version.h>
#include <core/arch/boot.h>
#include <core/arch/heap.h>
#include <core/dev/console.h>
#include <core/kprintf.h>
#include <core/klog.h>
#include <core/bus.h>
#include <core/ext.h>
#include <core/config.h>
#include <flanterm.h>
#include <flanterm_backends/fb.h>
#include <ext/assets/fonts.h>
#include <ext/fbcon/fbcon.h>
#include <ext/fbcon/fbcon_brand.h>
#include <ext/fbcon/fbcon_manifest.h>

static struct flanterm_context *term;
static struct limine_framebuffer *fb;

struct flanterm_context *fbcon_get_term(void) { return term; }
struct limine_framebuffer *fbcon_get_fb(void)  { return fb;  }

static uint32_t theme_bg = SYM_FBCON_BG;
static uint32_t theme_fg = SYM_FBCON_FG;
uint32_t fbcon_theme_bg(void) { return theme_bg; }
uint32_t fbcon_theme_fg(void) { return theme_fg; }

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

static int fbcon_allocate(void) {
    fb = boot_framebuffer();
    if (fb == NULL) return 1;
    if (fb->memory_model != LIMINE_FRAMEBUFFER_RGB || fb->bpp != 32) return 2;

    term = flanterm_fb_init(
        kmalloc, NULL,
        fb->address, fb->width, fb->height, fb->pitch,
        fb->red_mask_size, fb->red_mask_shift,
        fb->green_mask_size, fb->green_mask_shift,
        fb->blue_mask_size, fb->blue_mask_shift,
        NULL,
        NULL, NULL,
        &theme_bg, &theme_fg,
        NULL, NULL,
        (void *)cp850_8x16_font, CP850_8X16_WIDTH, CP850_8X16_HEIGHT,
        1, 0, 0, 0,
        FLANTERM_FB_ROTATE_0,
        true);
    if (term == NULL) return 3;

    console_register(&fbcon_ops);            /* replays the boot log */

    fbcon_publish_manifest(fb, term);        /* env: kernel.dev.console.* */
    fbcon_draw_logo(fb);                     /* branding overlay */

    return 0;
}

static int fbcon_init(void) { return fbcon_allocate(); }

SYM_EXTENSION(fbcon, fbcon_init, EXT_PRIO_CONSOLE);
