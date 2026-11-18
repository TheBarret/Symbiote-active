#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <core/klog.h>
#include <core/version.h>
#include <core/dev/console.h>

/* codebase: 0.2.0 */
/* Console driver and sink
 *
 * consumers:
 * - arch/serial.h
 * - ext/fbcon/fbcon.c
 */

static const struct console_sink *consoles[CONSOLE_MAX];
static size_t console_count;

/* Ring buffer for retaining recent kernel log history. */
static char log_buf[LOG_SIZE];
static size_t log_head;
static bool log_wrapped;

int console_register(const struct console_sink *sink) {
    if (sink == NULL) {
        return CONSOLE_INVALID;
    }
    if (console_count >= CONSOLE_MAX) {
        return CONSOLE_FULL;
    }

    consoles[console_count++] = sink;

    /* Replay retained history, oldest bytes first. */
    if (log_wrapped) {
        sink->write(log_buf + log_head, LOG_SIZE - log_head);
    }
    sink->write(log_buf, log_head);
    return CONSOLE_OK;
}

void console_write(const char *buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
        log_buf[log_head] = buf[i];
        if (++log_head == LOG_SIZE) {
            log_head = 0;
            log_wrapped = true;
        }
    }
    for (size_t i = 0; i < console_count; i++) {
        consoles[i]->write(buf, len);
    }
}

void console_putc(char c) { console_write(&c, 1); }
