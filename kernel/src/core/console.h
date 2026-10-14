#ifndef CORE_CONSOLE_H
#define CORE_CONSOLE_H

#include <stddef.h>
#include <core/bus.h>

/* Plugin console module */
struct console_ops {
    const char *name;
    void (*write)(const char *buf, size_t len);
};

/* init (bus back-end) */
void console_init(void);

/* Attach a console */
void console_register(const struct console_ops *ops);

/* Send text to every attached console (and into the boot log). */
void console_write(const char *buf, size_t len);

/* Send a single character to every attached console.
 * Cheap path for interactive echo; does NOT go through kprintf. */
void console_putc(char c);

#endif
