#include <stddef.h>
#include <stdint.h>
#include <core/version.h>
#include <core/cpu.h>
#include <core/console.h>
#include <core/serial.h>

// Moved to ref: core/version.h
//#define COM1 0x3F8

static void serial_putc(char c) {
    /* Wait (bounded) until the transmit holding register is empty.
     * A missing port reads back 0xFF, which also looks "ready", so this cannot hang. */
    for (int spin = 0; spin < 100000; spin++) {
        if (inb(COM1 + 5) & 0x20)
            break;
    }
    outb(COM1, (uint8_t)c);
}

static void serial_write(const char *buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (buf[i] == '\n')
            serial_putc('\r');      /* terminals want CR+LF */
        serial_putc(buf[i]);
    }
}

static const struct console_ops serial_ops = { "serial", serial_write };

void serial_init(void) {
    outb(COM1 + 1, 0x00);   /* no interrupts */
    outb(COM1 + 3, 0x80);   /* DLAB on: next two writes set the baud divisor */
    outb(COM1 + 0, 0x01);   /* divisor 1 = 115200 baud */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);   /* DLAB off, 8 data bits, no parity, 1 stop bit */
    outb(COM1 + 2, 0xC7);   /* enable and clear FIFOs */
    outb(COM1 + 4, 0x0B);   /* DTR + RTS + OUT2 */
    console_register(&serial_ops);
}
