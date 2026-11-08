#ifndef EXT_FBCON_FBCON_H
#define EXT_FBCON_FBCON_H

#include <stdint.h>
#include <stddef.h>
#include <flanterm.h>

struct limine_framebuffer;

/* Live objects owned by fbcon.c after successful init.
 * NULL before init; never freed (fbcon is a boot-time extension). */
struct flanterm_context *fbcon_get_term(void);
struct limine_framebuffer *fbcon_get_fb(void);

/* Theme colours (from config.h), exposed so branding and any future
 * overlays share the same palette as the terminal. */
uint32_t fbcon_theme_bg(void);
uint32_t fbcon_theme_fg(void);

#endif
