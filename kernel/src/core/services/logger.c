#include <stdarg.h>
#include <core/version.h>
#include <core/dev/vfs.h>
#include <core/kprintf.h>
#include <core/klog.h>
#include <core/dev/timer.h>
#include <core/services/logger.h>

static bool              e_warned = false;
static bool              e_open = false;
static struct vfs_file  *e_node = NULL;
static char              log_buffer[LOGGER_MAX_SIZE];

bool log_open(const char *path) {
    e_node = vfs_open(path, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_APPEND);
    if (e_node == NULL) {
        e_open = false;
        return false;
    }
    e_open = true;
    /* stamp the open event too; goes through the same path as everything else */
    log_write("log_open(path=%s)\n", path);
    return true;
}

static bool log_isvalid(void) {
    if (!e_open || !e_node)
        return false;

    struct vfs_stat st;
    if (vfs_stat("/kernel.log", &st) != 0)
        return false;                 /* path gone — node is dead, writes go nowhere */
    if (st.type != VFS_FILE)
        return false;                 /* path replaced by a directory or something */

    return true;
}

bool log_close(void) {
    if (!e_open) return false;
    vfs_close(e_node);
    e_open = false;
    e_node = NULL;
    return true;
}

int log_write(const char *fmt, ...) {

    if (!log_isvalid()) {
        if (!e_warned) {
            klog_error("logger.c: file node disappeared\n");
            log_close();
            e_warned = true;
        }
        return -1;
    }

    /* stamp */
    int off = ksnprintf(log_buffer, sizeof log_buffer,
                        "[%lu] ", (unsigned long)timer_ticks());
    if (off < 0) return -1;
    if (off >= (int)sizeof log_buffer) off = sizeof log_buffer - 1;

    /* formatter */
    va_list ap;
    va_start(ap, fmt);
    int body = kvsnprintf(log_buffer + off, sizeof log_buffer - (size_t)off, fmt, ap);
    va_end(ap);

    if (body < 0) return -1;
    size_t total = (size_t)off + (size_t)body;
    if (total >= sizeof log_buffer) {
        total = sizeof log_buffer - 1;
        log_buffer[total - 1] = '~';   /* visible truncation marker */
    }

    long w = vfs_write(e_node, log_buffer, total);
    return (w == (long)total) ? 0 : -1;
}
