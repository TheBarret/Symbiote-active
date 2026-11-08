#ifndef EXT_FBCON_MANIFEST_H
#define EXT_FBCON_MANIFEST_H

struct limine_framebuffer;
struct flanterm_context;

/* Publishes kernel.dev.console.{width,height,cols,rows} to env.
 * Ownership: BUS_CONSOLE_ID, PINNED | READONLY.
 * No-op if bus_env() is unavailable. Logs warnings on env failure,
 * never fails the caller. */
void fbcon_publish_manifest(struct limine_framebuffer *fb,
                            struct flanterm_context *term);

#endif
