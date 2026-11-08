#include <stddef.h>
#include <stdint.h>
#include <core/arch/boot.h>
#include <flanterm_backends/fb.h>
#include <ext/assets/logo.h>
#include <ext/fbcon/fbcon.h>
#include <ext/fbcon/fbcon_brand.h>

/* Pre-blend contract for logo.h:
 *   - logo_data[] is LOGO_WIDTH * LOGO_HEIGHT entries
 *   - each entry is 0x00RRGGBB (bit 31 = 0; alpha is already blended
 *     against theme_bg at build time)
 *   - no runtime alpha, no runtime format conversion
 *
 * The build-time blit is the source of truth; if you regenerate logo.h
 * from a PNG, pre-blend it against SYM_FBCON_BG. See assets/README. */

void fbcon_draw_logo(struct limine_framebuffer *fb) {
    if (fb == NULL) return;
    if (fb->width < LOGO_WIDTH || fb->height < LOGO_HEIGHT) return;
    if (fb->memory_model != LIMINE_FRAMEBUFFER_RGB || fb->bpp != 32) return;

    volatile uint32_t *fb_ptr = (volatile uint32_t *)fb->address;
    const uint32_t pitch_px = (uint32_t)(fb->pitch / 4);
    const uint32_t start_x  = (uint32_t)(fb->width - LOGO_WIDTH);
    const uint32_t start_y  = 0;

    for (uint32_t y = 0; y < LOGO_HEIGHT; y++) {
        const uint32_t *src_row = &logo_data[y * LOGO_WIDTH];
        uint32_t *dst_row = (uint32_t *)fb_ptr + (start_y + y) * pitch_px + start_x;
        for (uint32_t x = 0; x < LOGO_WIDTH; x++)
            dst_row[x] = src_row[x] & 0x00FFFFFFu;   /* mask any stray bit 31 */
    }
}
