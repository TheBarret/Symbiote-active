#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <core/version.h>
#include <core/arch/cpu.h>
#include <core/arch/serial.h>
#include <core/dev/console.h>
#include <core/klog.h>

/* codebase: 0.2.0 */

/* UART Register Offsets relative to COM1 base */
#define UART_THR      0 /* Transmitter Holding Register (write) */
#define UART_IER      1 /* Interrupt Enable Register */
#define UART_FCR      2 /* FIFO Control Register */
#define UART_LCR      3 /* Line Control Register */
#define UART_MCR      4 /* Modem Control Register */
#define UART_LSR      5 /* Line Status Register */

/* Line Status Register Bits */
#define UART_LSR_THRE 0x20 /* Transmit Holding Register Empty */

static void serial_outb(char c) {
    /* Wait (bounded) until the transmit holding register is empty.
     * A missing port reads back 0xFF, which also looks "ready", preventing a hang. */
    for (int spin = 0; spin < SERIAL_TX_HOLD; spin++) {
        if (inb(COM1 + UART_LSR) & UART_LSR_THRE) {
            break;
        }
    }
    outb(COM1 + UART_THR, (uint8_t)c);
}

static void serial_write(const char *buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (buf[i] == '\n') {
            serial_outb('\r'); /* Terminals require CR+LF translation */
        }
        serial_outb(buf[i]);
    }
}

static const struct console_sink serial_sink = {
    .name  = "serial",
    .write = serial_write
};

bool serial_init(void) {
    klog("→ serial_init() thr=%u, ier=%u, fcr=%u, lcr=%u, mcr=%u, lsr=%u, lsr_thr=%u\n",
        UART_THR,UART_IER,UART_FCR,UART_LCR,UART_MCR, UART_LSR, UART_LSR_THRE);

    outb(COM1 + UART_IER, 0x00); /* Disable all UART interrupts */
    outb(COM1 + UART_LCR, 0x80); /* Enable DLAB to configure baud divisor */
    outb(COM1 + 0,        0x01); /* Divisor low byte: 115200 baud (115200 / 1) */
    outb(COM1 + UART_IER, 0x00); /* Divisor high byte: 0x00 */
    outb(COM1 + UART_LCR, 0x03); /* Disable DLAB, 8 data bits, no parity, 1 stop bit */
    outb(COM1 + UART_FCR, 0xC7); /* Enable and clear FIFOs, 14-byte threshold */
    outb(COM1 + UART_MCR, 0x0B); /* Enable DTR, RTS, and OUT2 */

    if (console_register(&serial_sink) != CONSOLE_OK) {
        klog_error("Error: failed to register console sink\n");
        return false;
    }

    return true;
}
