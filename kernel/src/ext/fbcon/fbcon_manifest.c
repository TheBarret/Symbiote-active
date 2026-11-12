#include <stddef.h>
#include <stdint.h>
#include <core/arch/boot.h>
#include <core/dev/env.h>
#include <core/version.h>
#include <core/kprintf.h>
#include <core/klog.h>
#include <core/bus.h>
#include <core/mask.h>
#include <flanterm.h>
#include <flanterm_backends/fb.h>
#include <ext/fbcon/fbcon_manifest.h>

static void set_int(const struct env_ops *env, const char *ns, const char *name, size_t v, uint32_t mask) {
    char buf[16];
    ksnprintf(buf, sizeof buf, "%zu", v);
    int rc = env->set(ns, name, buf, 0, 0, 10, mask);
    if (rc != ENV_OK)
        klog_warning("fbcon_manifest(env->set %s): exit code = %d", name, rc);
}

void fbcon_publish_manifest(struct limine_framebuffer *fb, struct flanterm_context *term) {
    const struct env_ops *env = bus_env();
    if (env == NULL || fb == NULL || term == NULL) return;

    // appends to existing manifest namespace
    const char ns[] = "console";

    // BUS_CONSOLE_ID owns these symbols
    uint32_t m = env_mask_build(BUS_CONSOLE_ID, ENV_TYPE_INT, ENV_FLAG_PINNED | ENV_FLAG_READONLY, 0);

    size_t cols = 0, rows = 0;
    flanterm_get_dimensions(term, &cols, &rows);

    set_int(env, ns, "width",  (size_t)fb->width,  m);
    set_int(env, ns, "height", (size_t)fb->height, m);
    set_int(env, ns, "cols",   cols,               m);
    set_int(env, ns, "rows",   rows,               m);
}
