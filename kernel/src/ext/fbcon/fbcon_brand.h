#ifndef EXT_FBCON_BRAND_H
#define EXT_FBCON_BRAND_H

struct limine_framebuffer;

/* Draws the branding logo into the top-right corner of the framebuffer.
 * No-op if the framebuffer is smaller than the logo.
 * Must be called after flanterm has drawn the boot log (so the logo
 * overlays the top-right corner and doesn't get overwritten). */
void fbcon_draw_logo(struct limine_framebuffer *fb);

#endif
