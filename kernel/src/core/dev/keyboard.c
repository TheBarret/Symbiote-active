#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <core/version.h>
#include <core/arch/cpu.h>
#include <core/arch/isr.h>
#include <core/dev/env.h>
#include <core/dev/keyboard.h>
#include <core/dev/timer.h>
#include <core/bus.h>
#include <core/mask.h>
#include <core/kprintf.h>
#include <core/klog.h>

/* codebase: 0.1.2 */

/* Controller ports and status bits */
#define KBD_DATA    0x60
#define KBD_STATUS  0x64
#define KBD_COMMAND 0x64

#define STATUS_OUTPUT_FULL  0x01
#define STATUS_INPUT_FULL   0x02
#define STATUS_AUXBUF       0x20

#define CMD_READ_CONFIG     0x20
#define CMD_WRITE_CONFIG    0x60
#define CMD_DISABLE_KBD     0xAD
#define CMD_ENABLE_KBD      0xAE
#define CMD_SELF_TEST       0xAA
#define CMD_TEST_KBD        0xAB
#define CMD_WRITE_KBD       0xD4

#define KBD_CMD_SET_SCANCODE  0xF0
#define KBD_CMD_ENABLE        0xF4
#define KBD_CMD_LED           0xED

#define KBD_ACK         0xFA
#define KBD_RESEND      0xFE
#define KBD_SELFTEST_OK 0x55

#define KBD_SPIN_LIMIT 5000 /* 5000 - 10k cycles */

#define KBD_RING_SIZE 256

#define KBD_PUBLISH_MS 250 /* 100 - 250 ticks */

static uint8_t  ring_buf[KBD_RING_SIZE];
static uint32_t ring_head;
static uint32_t ring_tail;
static bool     irq_hooked;

// Keyboards own metric endpoints
static volatile uint64_t kbd_keystrokes_total;   /* every scancode decoded (make and break) */
static volatile uint64_t kbd_chars_total;        /* only KBD_CHAR events */
static volatile uint64_t kbd_dropped_of;            /* ring_push overflowed */
static volatile uint8_t  kbd_locks_mod;              /* KBD_LOCK_CAPS | NUM | SCROLL */
static volatile uint8_t  kbd_last_char;          /* last KBD_CHAR out-value */
static volatile uint8_t  kbd_last_scancode;      /* last raw make-code seen */
static volatile uint64_t kbd_last_ticks;         /* tick at last event */


static bool wait_input_clear(void) {
    for (int i = 0; i < KBD_SPIN_LIMIT; i++) {
        if (!(inb(KBD_STATUS) & STATUS_INPUT_FULL))
            return true;
    }
    return false;
}

static bool wait_output_full(void) {
    for (int i = 0; i < KBD_SPIN_LIMIT; i++) {
        if (inb(KBD_STATUS) & STATUS_OUTPUT_FULL)
            return true;
    }
    return false;
}

static bool write_command(uint8_t cmd) {
    if (!wait_input_clear())
        return false;
    outb(KBD_COMMAND, cmd);
    return true;
}

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

static bool read_byte(uint8_t *out) {
    if (!wait_output_full())
        return false;
    *out = inb(KBD_DATA);
    return true;
}

static void drain(void) {
    for (int i = 0; i < 64; i++) {
        if (!(inb(KBD_STATUS) & STATUS_OUTPUT_FULL))
            break;
        (void)inb(KBD_DATA);
    }
}

static bool ring_empty(void) {
    return __atomic_load_n(&ring_head, __ATOMIC_ACQUIRE) ==
           __atomic_load_n(&ring_tail, __ATOMIC_ACQUIRE);
}

static void ring_push(uint8_t b) {
    uint32_t head = ring_head;
    uint32_t next = (head + 1) % KBD_RING_SIZE;
    if (next == __atomic_load_n(&ring_tail, __ATOMIC_ACQUIRE)) {
        kbd_dropped_of++;               /* ENV overrun counter */
        return;
    }
    ring_buf[head] = b;
    __atomic_store_n(&ring_head, next, __ATOMIC_RELEASE);
}

static bool ring_pop(uint8_t *out) {
    uint32_t tail = ring_tail;
    if (tail == __atomic_load_n(&ring_head, __ATOMIC_ACQUIRE))
        return false;
    *out = ring_buf[tail];
    __atomic_store_n(&ring_tail, (tail + 1) % KBD_RING_SIZE, __ATOMIC_RELEASE);
    return true;
}

static void kbd_irq(void *ctx) {
    (void)ctx;
    for (int i = 0; i < 16; i++) {
        uint8_t status = inb(KBD_STATUS);
        if (!(status & STATUS_OUTPUT_FULL))
            break;
        uint8_t b = inb(KBD_DATA);
        if (status & STATUS_AUXBUF)
            continue;
        kbd_keystrokes_total++;     /* ENV count every byte */
        kbd_last_scancode = b;      /* ENV raw make/break code */
        kbd_last_ticks = timer_ticks();
        ring_push(b);
    }
}

/* LED synchronization */
static bool shift_held;
static bool caps_lock;
static bool num_lock = true;
static bool scroll_lock;
static bool extended;
static int  e1_state = 0;

static void sync_locks_to_stats(void) {
    kbd_locks_mod = (caps_lock ? KBD_LOCK_CAPS : 0)
                    | (num_lock  ? KBD_LOCK_NUM  : 0)
                    | (scroll_lock ? KBD_LOCK_SCROLL : 0);
}

static void update_leds(void) {
    uint8_t led_bits = (uint8_t)((caps_lock << 2) | (num_lock << 1) | scroll_lock);
    (void)write_keyboard(KBD_CMD_LED);
    (void)write_keyboard(led_bits);
    sync_locks_to_stats();                /* ENV keep the snapshot current */
}

bool kbd_init(void) {
    klog("→ keyboard_init()\n");
    drain();
    if (!write_command(CMD_DISABLE_KBD))
        return false;
    drain();
    if (!write_command(CMD_SELF_TEST))
        return false;

    uint8_t result = 0;
    if (!read_byte(&result) || result != KBD_SELFTEST_OK)
        return false;

    if (!write_command(CMD_READ_CONFIG))
        return false;

    uint8_t config = 0;
    if (!read_byte(&config))
        return false;

    config &= (uint8_t)~0x10;
    config |= 0x40;
    config |= 0x01;

    if (!write_command(CMD_WRITE_CONFIG))
        return false;
    if (!wait_input_clear())
        return false;
    outb(KBD_DATA, config);

    if (!write_command(CMD_ENABLE_KBD))
        return false;

    if (!write_keyboard(KBD_CMD_ENABLE))
        return false;

    drain();

    if (!irq_hooked) {
        if (irq_register(1, kbd_irq, NULL) != 0)
            return false;
        irq_hooked = true;
    }

    update_leds();

    /* ENV register on the bus */

    static const struct kbd_ops g_kbd_ops = { .locks = kbd_locks, .dropped = kbd_dropped };

    struct bus_device dev = {
            .name = "keyboard",
            .class = BUS_CLASS_KEYBOARD,
            .version = 1,
            .ops = &g_kbd_ops,
            .private_data = NULL,
            .owner_id = BUS_KBD_ID,
        };
        if (bus_register(&dev) != 0) {
            klog_warning("keyboard: bus registration failed\n");
        } else {
            bus_test("keyboard");
        }
    return true;
}

/* Scancode tables (set 1) */
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
    case 0x37: return KBD_KEY_PRINTSCREEN;
    default:   return 0;
    }
}

enum kbd_event kbd_poll(char *out) {
    uint8_t sc;
    if (!ring_pop(&sc))
        return KBD_NONE;

    if (e1_state > 0) {
        e1_state--;
        if (e1_state == 0) {
            *out = (char)KBD_KEY_PAUSE;
            return KBD_SPECIAL;
        }
        return KBD_NONE;
    }
    if (sc == 0xE1) { e1_state = 5; return KBD_NONE; }
    if (sc == 0xE0) { extended = true; return KBD_NONE; }

    bool release = (sc & 0x80) != 0;
    uint8_t make = sc & 0x7F;

    if (extended) {
        extended = false;
        int key = extended_key(make);
        if (key) {
            *out = (char)key;
            return release ? KBD_NONE : KBD_SPECIAL;
        }
        *out = (char)make;
        return KBD_UNKNOWN;
    }

    if (make == 0x2A || make == 0x36) {
        shift_held = !release;
        *out = (char)(make == 0x2A ? KBD_KEY_LSHIFT : KBD_KEY_RSHIFT);
        return KBD_SPECIAL;
    }
    if (make == 0x1D) { *out = (char)KBD_KEY_LCTRL; return KBD_SPECIAL; }
    if (make == 0x38) { *out = (char)KBD_KEY_LALT;  return KBD_SPECIAL; }
    if (make == 0x3A && !release) {
        caps_lock = !caps_lock;
        update_leds();
        *out = (char)KBD_KEY_CAPSLOCK; return KBD_SPECIAL;
    }
    if (make == 0x45 && !release) {
        num_lock = !num_lock;
        update_leds();
        *out = (char)KBD_KEY_NUMLOCK; return KBD_SPECIAL;
    }
    if (make == 0x46 && !release) {
        scroll_lock = !scroll_lock;
        update_leds();
        *out = (char)KBD_KEY_SCROLLLOCK; return KBD_SPECIAL;
    }
    if (make == 0x01) { *out = (char)KBD_KEY_ESC; return KBD_SPECIAL; }
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
    if (caps_lock && !shift_held && c >= 'a' && c <= 'z')
        c = (char)(c - 'a' + 'A');
    else if (caps_lock && shift_held && c >= 'A' && c <= 'Z')
        c = (char)(c - 'A' + 'a');

    *out = c;
    kbd_chars_total++;              /* ENV printable chars */
    kbd_last_char = (uint8_t)c;     /* ENV last printable */
    kbd_last_ticks = timer_ticks();
    return KBD_CHAR;
}

char kbd_getchar(void) {
    for (;;) {
        char c = 0;
        if (kbd_poll(&c) == KBD_CHAR)
            return c;

        /* Just sleep until the next interrupt (timer OR keyboard).
         * The super-loop in kmain() owns timer_drain(). */
        uint64_t flags = cpu_irq_save();
        if (ring_empty()) {
            cpu_sti_hlt();
        }
        cpu_irq_restore(flags);
    }
}

enum kbd_event kbd_wait(char *out) {
    for (;;) {
        enum kbd_event ev = kbd_poll(out);
        if (ev != KBD_NONE)
            return ev;

        /* Just sleep until the next interrupt. */
        uint64_t flags = cpu_irq_save();
        if (ring_empty()) {
            cpu_sti_hlt();
        }
        cpu_irq_restore(flags);
    }
}

/* ENV live reads (thread context) */

uint8_t kbd_locks(void) {
    return kbd_locks_mod;
}

uint32_t kbd_dropped(void) {
    return (uint32_t)kbd_dropped_of;
}
