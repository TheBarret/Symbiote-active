#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <core/arch/cpu.h>
#include <core/arch/isr.h>
#include <core/dev/keyboard.h>
#include <core/dev/timer.h>
#include <core/kprintf.h>

/* Controller ports and status bits */
#define KBD_DATA    0x60
#define KBD_STATUS  0x64
#define KBD_COMMAND 0x64

#define STATUS_OUTPUT_FULL  0x01    /* a byte is ready at 0x60 */
#define STATUS_INPUT_FULL   0x02    /* controller is busy; do not write */
#define STATUS_AUXBUF       0x20    /* waiting byte is from the mouse */

/* Controller commands */
#define CMD_READ_CONFIG     0x20
#define CMD_WRITE_CONFIG    0x60
#define CMD_DISABLE_KBD     0xAD
#define CMD_ENABLE_KBD      0xAE
#define CMD_SELF_TEST       0xAA
#define CMD_TEST_KBD        0xAB
#define CMD_WRITE_KBD       0xD4

/* Keyboard commands */
#define KBD_CMD_SET_SCANCODE  0xF0
#define KBD_CMD_ENABLE        0xF4
#define KBD_CMD_LED           0xED

/* Responses */
#define KBD_ACK     0xFA
#define KBD_RESEND  0xFE
#define KBD_SELFTEST_OK 0x55

#define KBD_SPIN_LIMIT 100000

/* Ring Buffer */
#define KBD_RING_SIZE 256

static uint8_t  ring_buf[KBD_RING_SIZE];
static uint32_t ring_head;
static uint32_t ring_tail;
static bool     irq_hooked;


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
    if (next == __atomic_load_n(&ring_tail, __ATOMIC_ACQUIRE))
        return;
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
        ring_push(b);
    }
}

/* LED synchronization */
static bool shift_held;
static bool caps_lock;
static bool num_lock = true;   /* standard default on boot */
static bool scroll_lock;
static bool extended;
static int  e1_state = 0;      /* tracks the 6-byte 0xE1 pause sequence */

static void update_leds(void) {
    uint8_t led_bits = (caps_lock << 2) | (num_lock << 1) | scroll_lock;
    (void)write_keyboard(KBD_CMD_LED);
    (void)write_keyboard(led_bits);
}

bool kbd_init(void) {
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

    config &= (uint8_t)~0x10;   /* clear disable-clock bit */
    config |= 0x40;             /* translation set 1 */
    config |= 0x01;             /* enable IRQ1 */

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

    /* Handle 6-byte Pause/Break sequence (0xE1 ...) */
    if (e1_state > 0) {
        e1_state--;
        if (e1_state == 0) {
            *out = (char)KBD_KEY_PAUSE;
            return KBD_SPECIAL;
        }
        return KBD_NONE;
    }
    if (sc == 0xE1) {
        e1_state = 5; /* skip next 5 bytes of pause sequence */
        return KBD_NONE;
    }

    /* Handle PrintScreen fake prefix sequence workaround if needed */
    if (sc == 0xE0) {
        extended = true;
        return KBD_NONE;
    }

    bool release = (sc & 0x80) != 0;
    uint8_t make = sc & 0x7F;

    if (extended) {
        extended = false;
        int key = extended_key(make);
        if (key) {
            *out = (char)key;
            return release ? KBD_NONE : KBD_SPECIAL; /* track extended release or report special */
        }
        *out = (char)make;
        return KBD_UNKNOWN;
    }

    /* Modifiers & toggles */
    if (make == 0x2A || make == 0x36) {        /* left/right shift */
        shift_held = !release;
        *out = (char)(make == 0x2A ? KBD_KEY_LSHIFT : KBD_KEY_RSHIFT);
        return KBD_SPECIAL;
    }
    if (make == 0x1D) {                        /* left ctrl */
        *out = (char)KBD_KEY_LCTRL;
        return KBD_SPECIAL;
    }
    if (make == 0x38) {                        /* left alt */
        *out = (char)KBD_KEY_LALT;
        return KBD_SPECIAL;
    }
    if (make == 0x3A && !release) {            /* caps lock */
        caps_lock = !caps_lock;
        update_leds();
        *out = (char)KBD_KEY_CAPSLOCK;
        return KBD_SPECIAL;
    }
    if (make == 0x45 && !release) {            /* num lock */
        num_lock = !num_lock;
        update_leds();
        *out = (char)KBD_KEY_NUMLOCK;
        return KBD_SPECIAL;
    }
    if (make == 0x46 && !release) {            /* scroll lock */
        scroll_lock = !scroll_lock;
        update_leds();
        *out = (char)KBD_KEY_SCROLLLOCK;
        return KBD_SPECIAL;
    }
    if (make == 0x01) {                        /* escape */
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

    if (caps_lock && !shift_held && c >= 'a' && c <= 'z')
        c = (char)(c - 'a' + 'A');
    else if (caps_lock && shift_held && c >= 'A' && c <= 'Z')
        c = (char)(c - 'A' + 'a');

    *out = c;
    return KBD_CHAR;
}

char kbd_getchar(void) {
    for (;;) {
        char c = 0;
        if (kbd_poll(&c) == KBD_CHAR)
            return c;

        uint64_t flags = cpu_irq_save();
        while (ring_empty()) {
            cpu_sti_hlt();
            cpu_cli();
            timer_drain();
        }
        cpu_irq_restore(flags);
    }
}

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
