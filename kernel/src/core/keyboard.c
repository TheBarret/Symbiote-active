#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <core/cpu.h>
#include <core/isr.h>
#include <core/keyboard.h>
#include <core/kprintf.h>
#include <core/timer.h>

/*  controller ports and status bits  */

#define KBD_DATA    0x60
#define KBD_STATUS  0x64
#define KBD_COMMAND 0x64

#define STATUS_OUTPUT_FULL  0x01    /* a byte is ready at 0x60 */
#define STATUS_INPUT_FULL   0x02    /* controller is busy; do not write */
#define STATUS_AUXBUF       0x20    /* waiting byte is from the mouse */

/* Controller commands (written to 0x64). */
#define CMD_READ_CONFIG     0x20
#define CMD_WRITE_CONFIG    0x60
#define CMD_DISABLE_KBD     0xAD
#define CMD_ENABLE_KBD      0xAE
#define CMD_SELF_TEST       0xAA
#define CMD_TEST_KBD        0xAB
#define CMD_WRITE_KBD       0xD4      /* next byte goes to the keyboard, not the controller */

/* Keyboard commands (written to 0x60 after CMD_WRITE_KBD). */
#define KBD_CMD_SET_SCANCODE  0xF0
#define KBD_CMD_ENABLE        0xF4

/* Responses. */
#define KBD_ACK   0xFA
#define KBD_RESEND 0xFE

/* The self-test expects this byte back. */
#define KBD_SELFTEST_OK 0x55

/*  bounded I/O helpers
 *
 * The controller is not fast, but it is not slow enough to justify an infinite loop.
 * A bounded spin converts "controller is wedged" from "kernel hangs" into "we log and give up",
 * which is the correct failure mode for a boot-time driver.
 * 100000 iterations of inb() is well over the worst-case latency of a real controller. */

#define KBD_SPIN_LIMIT 100000

/* Wait until the controller's input buffer is empty (bit 1 of 0x64 clear).
 * Returns false on timeout. */
static bool wait_input_clear(void) {
    for (int i = 0; i < KBD_SPIN_LIMIT; i++) {
        if (!(inb(KBD_STATUS) & STATUS_INPUT_FULL))
            return true;
    }
    return false;
}

/* Wait until the controller has a byte for us (bit 0 of 0x64 set).
 * Returns false on timeout. */
static bool wait_output_full(void) {
    for (int i = 0; i < KBD_SPIN_LIMIT; i++) {
        if (inb(KBD_STATUS) & STATUS_OUTPUT_FULL)
            return true;
    }
    return false;
}

/* Write one byte to the controller at 0x64. Returns false on timeout. */
static bool write_command(uint8_t cmd) {
    if (!wait_input_clear())
        return false;
    outb(KBD_COMMAND, cmd);
    return true;
}

/* Write one byte to the keyboard itself (via the controller).
 * Uses the 0xD4 prefix so the byte goes to the device, not the controller.
 * Consumes and checks the ACK. Returns false on timeout or unexpected reply. */
static bool write_keyboard(uint8_t data) {
    if (!write_command(CMD_WRITE_KBD))
        return false;
    if (!wait_input_clear())
        return false;
    outb(KBD_DATA, data);

    if (!wait_output_full())
        return false;
    uint8_t resp = inb(KBD_DATA);
    return resp == KBD_ACK;
}

/* Read one byte the controller has queued. Returns false on timeout. */
static bool read_byte(uint8_t *out) {
    if (!wait_output_full())
        return false;
    *out = inb(KBD_DATA);
    return true;
}

/* Drain the controller's output buffer. Any bytes queued before the kernel took over (including keystrokes typed during boot) are dropped. */
static void drain(void) {
    for (int i = 0; i < 64; i++) {
        if (!(inb(KBD_STATUS) & STATUS_OUTPUT_FULL))
            break;
        (void)inb(KBD_DATA);
    }
}

/*  IRQ side: raw bytes into a ring buffer
 *
 * Single producer (the IRQ1 handler), single consumer (kbd_poll).
 * Indices are accessed with acquire/release atomics so neither side needs a lock.
 * When the ring is full new bytes are dropped: losing a keystroke beats corrupting one. */

#define KBD_RING_SIZE 128       /* bytes */

static uint8_t  ring_buf[KBD_RING_SIZE];
static uint32_t ring_head;      /* next slot the IRQ writes */
static uint32_t ring_tail;      /* next slot kbd_poll reads */
static bool     irq_hooked;

static bool ring_empty(void) {
    return __atomic_load_n(&ring_head, __ATOMIC_ACQUIRE) ==
           __atomic_load_n(&ring_tail, __ATOMIC_ACQUIRE);
}

static void ring_push(uint8_t b) {                  /* IRQ context */
    uint32_t head = ring_head;
    uint32_t next = (head + 1) % KBD_RING_SIZE;
    if (next == __atomic_load_n(&ring_tail, __ATOMIC_ACQUIRE))
        return;                                      /* full: drop */
    ring_buf[head] = b;
    __atomic_store_n(&ring_head, next, __ATOMIC_RELEASE);
}

static bool ring_pop(uint8_t *out) {                /* thread context */
    uint32_t tail = ring_tail;
    if (tail == __atomic_load_n(&ring_head, __ATOMIC_ACQUIRE))
        return false;
    *out = ring_buf[tail];
    __atomic_store_n(&ring_tail, (tail + 1) % KBD_RING_SIZE, __ATOMIC_RELEASE);
    return true;
}

/* IRQ1. Reads port 0x60 on EVERY interrupt: if the byte is not consumed the controller stops raising IRQ1.
 * Mouse (aux) bytes are not ours; they are read and dropped. No printing, no allocation here. */
static void kbd_irq(void *ctx) {
    (void)ctx;
    for (int i = 0; i < 16; i++) {
        uint8_t status = inb(KBD_STATUS);
        if (!(status & STATUS_OUTPUT_FULL))
            break;
        uint8_t b = inb(KBD_DATA);
        if (status & STATUS_AUXBUF)
            continue;
        ring_push(b);
    }
}

/*  init  */

bool kbd_init(void) {
    /* 1. Drain anything the firmware left behind. */
    drain();

    /* 2. Disable both devices while we reconfigure. */
    if (!write_command(CMD_DISABLE_KBD))
        return false;

    /* 3. Flush again; disabling can produce a byte. */
    drain();

    /* 4. Controller self-test. */
    if (!write_command(CMD_SELF_TEST))
        return false;

    uint8_t result = 0;
    if (!read_byte(&result))
        return false;
    if (result != KBD_SELFTEST_OK)
        return false;

    /* 5. Read the configuration byte. We do not need to preserve every bit,
     * but we do need to know whether the keyboard clock is disabled before we re-enable the device. */
    if (!write_command(CMD_READ_CONFIG))
        return false;

    uint8_t config = 0;
    if (!read_byte(&config))
        return false;

    /* Clear bit 4 (disable keyboard clock) and bit 6 (translation to scancode set 1).
     * We will set translation off explicitly below so the device emits set 2 codes,
     * but we want a known starting state. */
    //config &= (uint8_t)~0x10; (broken)
    //config &= (uint8_t)~0x40; (broken)
    config &= (uint8_t)~0x10;   /* clear disable-clock bit */
    config |= 0x40;             /* set translation: the controller will
                                 * convert whatever the device emits into
                                 * set 1 codes, matching our tables. */
    config |= 0x01;             /* enable the keyboard interrupt (IRQ1) */

    if (!write_command(CMD_WRITE_CONFIG))
        return false;
    if (!wait_input_clear())
        return false;
    outb(KBD_DATA, config);

    /* 6. Re-enable the keyboard device. */
    if (!write_command(CMD_ENABLE_KBD))
        return false;

    /* 7. Ask the device to use scancode set 1.
     * The controller is not translating (we cleared bit 6), this is what the driver sees.
     * If the device refuses we fall back to whatever it was already using;
     * the tables below are set 1, so this matters. */
    //(void)write_keyboard(KBD_CMD_SET_SCANCODE); (broken)
    //(void)write_keyboard(0x01);     /* set 1 */ (broken)

    /* 8. Enable scanning. */
    if (!write_keyboard(KBD_CMD_ENABLE))
        return false;

    /* 9. Flush any bytes generated by the enable command. */
    drain();

    /* 10. Only now take IRQ1. Everything above read the controller by polling,
     * and a live handler would have stolen those replies (the ACK bytes). */
    if (!irq_hooked) {
        if (irq_register(1, kbd_irq, NULL) != 0)
            return false;
        irq_hooked = true;
    }

    return true;
}

/*  scancode tables (set 1)  */

static const char sc_ascii[128] = {
    [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4', [0x06] = '5',
    [0x07] = '6', [0x08] = '7', [0x09] = '8', [0x0A] = '9', [0x0B] = '0',
    [0x0C] = '-', [0x0D] = '=', [0x0E] = '\b',
    [0x0F] = '\t',
    [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r', [0x14] = 't',
    [0x15] = 'y', [0x16] = 'u', [0x17] = 'i', [0x18] = 'o', [0x19] = 'p',
    [0x1A] = '[', [0x1B] = ']',
    [0x1C] = '\n',
    [0x1E] = 'a', [0x1F] = 's', [0x20] = 'd', [0x21] = 'f', [0x22] = 'g',
    [0x23] = 'h', [0x24] = 'j', [0x25] = 'k', [0x26] = 'l',
    [0x27] = ';', [0x28] = '\'', [0x29] = '`',
    [0x2B] = '\\',
    [0x2C] = 'z', [0x2D] = 'x', [0x2E] = 'c', [0x2F] = 'v', [0x30] = 'b',
    [0x31] = 'n', [0x32] = 'm',
    [0x33] = ',', [0x34] = '.', [0x35] = '/',
    [0x39] = ' ',
};

static const char sc_ascii_shift[128] = {
    [0x02] = '!', [0x03] = '@', [0x04] = '#', [0x05] = '$', [0x06] = '%',
    [0x07] = '^', [0x08] = '&', [0x09] = '*', [0x0A] = '(', [0x0B] = ')',
    [0x0C] = '_', [0x0D] = '+', [0x0E] = '\b',
    [0x0F] = '\t',
    [0x10] = 'Q', [0x11] = 'W', [0x12] = 'E', [0x13] = 'R', [0x14] = 'T',
    [0x15] = 'Y', [0x16] = 'U', [0x17] = 'I', [0x18] = 'O', [0x19] = 'P',
    [0x1A] = '{', [0x1B] = '}',
    [0x1C] = '\n',
    [0x1E] = 'A', [0x1F] = 'S', [0x20] = 'D', [0x21] = 'F', [0x22] = 'G',
    [0x23] = 'H', [0x24] = 'J', [0x25] = 'K', [0x26] = 'L',
    [0x27] = ':', [0x28] = '"', [0x29] = '~',
    [0x2B] = '|',
    [0x2C] = 'Z', [0x2D] = 'X', [0x2E] = 'C', [0x2F] = 'V', [0x30] = 'B',
    [0x31] = 'N', [0x32] = 'M',
    [0x33] = '<', [0x34] = '>', [0x35] = '?',
    [0x39] = ' ',
};

/* Extended scancodes (after an 0xE0 prefix) that we recognize. */
static int extended_key(uint8_t sc) {
    switch (sc) {
    case 0x48: return KBD_KEY_UP;
    case 0x50: return KBD_KEY_DOWN;
    case 0x4B: return KBD_KEY_LEFT;
    case 0x4D: return KBD_KEY_RIGHT;
    case 0x47: return KBD_KEY_HOME;
    case 0x4F: return KBD_KEY_END;
    case 0x49: return KBD_KEY_PGUP;
    case 0x51: return KBD_KEY_PGDN;
    case 0x52: return KBD_KEY_INSERT;
    case 0x53: return KBD_KEY_DELETE;
    case 0x1D: return KBD_KEY_RCTRL;
    case 0x38: return KBD_KEY_RALT;
    default:   return 0;
    }
}

/*  state  */

static bool shift_held;
static bool caps_lock;
static bool extended;   /* last scancode was the 0xE0 prefix */

/*  the event producer  */

enum kbd_event kbd_poll(char *out) {
    //if (!(inb(KBD_STATUS) & STATUS_OUTPUT_FULL)) (wrong, it injects mouse delta bytes)
    //    return KBD_NONE;
    //uint8_t sc = inb(KBD_DATA);

    /* The IRQ handler already filtered out mouse bytes; what is in the ring is raw scancodes from the keyboard. */
    uint8_t sc;
    if (!ring_pop(&sc))
        return KBD_NONE;

    /* Extended prefix: remember it and consume this byte. */
    if (sc == 0xE0) {
        extended = true;
        return KBD_NONE;
    }

    /* Bit 7 set means the key was released. Update modifier state on release for Shift,
     * and report nothing, other modifier releases are also reported as KBD_SPECIAL,
     * so the shell can see them if it ever wants to; today it ignores them. */
    bool release = (sc & 0x80) != 0;
    uint8_t make = sc & 0x7F;

    if (extended) {
        extended = false;

        if (release)
            return KBD_NONE;    /* we do not track releases for extended keys */

        int key = extended_key(make);
        if (key) {
            *out = (char)key;
            return KBD_SPECIAL;
        }
        *out = (char)make;
        return KBD_UNKNOWN;
    }

    /* Non-extended modifier handling. */
    if (make == 0x2A || make == 0x36) {         /* left/right shift */
        shift_held = !release;
        *out = (char)(make == 0x2A ? KBD_KEY_LSHIFT : KBD_KEY_RSHIFT);
        return KBD_SPECIAL;
    }
    if (make == 0x1D) {                         /* left ctrl */
        *out = (char)KBD_KEY_LCTRL;
        return KBD_SPECIAL;
    }
    if (make == 0x38) {                         /* left alt */
        *out = (char)KBD_KEY_LALT;
        return KBD_SPECIAL;
    }
    if (make == 0x3A && !release) {             /* caps lock toggles on press */
        caps_lock = !caps_lock;
        *out = (char)KBD_KEY_CAPSLOCK;
        return KBD_SPECIAL;
    }
    if (make == 0x01) {                         /* escape */
        *out = (char)KBD_KEY_ESC;
        return KBD_SPECIAL;
    }
    if (make >= 0x3B && make <= 0x44 && !release) {
        *out = (char)(KBD_KEY_F1 + (make - 0x3B));
        return KBD_SPECIAL;
    }

    if (release)
        return KBD_NONE;

    const char *table = shift_held ? sc_ascii_shift : sc_ascii;
    char c = table[make];
    if (c == 0) {
        *out = (char)make;
        return KBD_UNKNOWN;
    }

    /* Caps Lock affects letters only. */
    if (caps_lock && !shift_held && c >= 'a' && c <= 'z')
        c = (char)(c - 'a' + 'A');
    else if (caps_lock && shift_held && c >= 'A' && c <= 'Z')
        c = (char)(c - 'A' + 'a');

    *out = c;
    return KBD_CHAR;
}

/*  Blocking reader  */

char kbd_getchar(void) {
    for (;;) {
        char c = 0;
        if (kbd_poll(&c) == KBD_CHAR)
            return c;
        /* Nothing usable yet: halt until an interrupt instead of spinning.
         * Interrupts stay off while the ring is checked,
         * and are enabled only for the hlt itself,
         * so a keystroke cannot arrive between "ring is empty" and "go to sleep" and be missed. */
        uint64_t flags = cpu_irq_save();
        while (ring_empty()) {
            cpu_sti_hlt();
            cpu_cli();
            timer_drain();   /* <- woke from any IRQ; run deferred work */
        }
        cpu_irq_restore(flags);
    }
}
/* Blocking until...  */
enum kbd_event kbd_wait(char *out) {
    for (;;) {
        enum kbd_event ev = kbd_poll(out);
        if (ev != KBD_NONE)
            return ev;

        uint64_t flags = cpu_irq_save();
        while (ring_empty()) {
            cpu_sti_hlt();
            cpu_cli();
        }
        cpu_irq_restore(flags);
    }
}
