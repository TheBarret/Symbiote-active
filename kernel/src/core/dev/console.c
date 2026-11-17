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
        return CONSOLE_INVAL;
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

    klog("→ sink added, length=%lu\n", console_count);
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

void console_putc(char c) {
    console_write(&c, 1);
}

static size_t log_retained(void) {
    return log_wrapped ? LOG_SIZE : log_head;
}

static bool log_read_byte(size_t off, uint8_t *out) {
    size_t n = log_retained();
    if (off >= n) {
        return false;
    }
    size_t idx = log_wrapped ? (log_head + off) % LOG_SIZE : off;
    *out = (uint8_t)log_buf[idx];
    return true;
}

static int console_isa_read(void *ctx, uint32_t slot, uint8_t width, uint32_t *out) {
    (void)ctx;
    if (width != ISA_W8) {
        return ISA_ERR_WIDTH;
    }
    uint8_t b = 0;
    if (!log_read_byte((size_t)slot, &b)) {
        return ISA_ERR;
    }
    *out = (uint32_t)b;
    return ISA_OK;
}

static int console_isa_write(void *ctx, uint32_t slot, uint8_t width, uint32_t in) {
    (void)ctx;
    (void)slot;
    if (width != ISA_W8) {
        return ISA_ERR_WIDTH;
    }
    console_putc((char)(in & 0xFFu));
    return ISA_OK;
}

static const struct isa_periph console_periph = {
    .name  = "console",
    .read  = console_isa_read,
    .write = console_isa_write,
    .ctx   = NULL,
};

/* ISA descriptor */
const struct isa_periph *get_console(void) { return &console_periph; }
