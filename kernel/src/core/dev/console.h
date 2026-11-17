#ifndef CORE_CONSOLE_H
#define CORE_CONSOLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <core/isa.h>

/* codebase: 0.2.0 */

/*
 * Console sink interface contract.
 * Sinks must have static lifetime; the console stores pointers, not copies.
 * The write callback must be byte-oriented, non-blocking, and re-entrant.
 */
struct console_sink {
    const char *name;
    void (*write)(const char *buf, size_t len);
};

/* Return status codes for console registration operations. */
enum console_status {
    CONSOLE_OK    = 0, /* Sink attached successfully, boot log replayed */
    CONSOLE_FULL  = 1, /* Attachment failed: sink capacity exceeded */
    CONSOLE_INVAL = 2, /* Attachment failed: invalid or NULL sink pointer */
};

/*
 * Registers an output sink. Replays the retained boot log to the sink
 * before appending future outputs.
 */
int console_register(const struct console_sink *sink);

/* Writes a byte stream to all registered sinks and appends it to the ring buffer log. */
void console_write(const char *buf, size_t len);

/* Convenience wrapper to write a single character via console_write. */
void console_putc(char c);

/* Retrieves the ISA peripheral descriptor for the console software bus binding. */
const struct isa_periph *get_console(void);

#endif /* CORE_CONSOLE_H */
