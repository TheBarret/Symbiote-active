#ifndef CORE_KEYBOARD_H
#define CORE_KEYBOARD_H

#include <stdbool.h>
#include <stdint.h>

/* Interrupt-driven PS/2 keyboard driver
 * - added LED sync
 * - multi-byte prefix state machine
 * - extended release tracking */

/* env-poller */
#define KBD_LOCK_CAPS 0x01
#define KBD_LOCK_NUM 0x02
#define KBD_LOCK_SCROLL 0x04
uint8_t kbd_locks(void);
uint32_t kbd_dropped(void);

/* key stroke/event */
enum kbd_event {
    KBD_NONE = 0,    /* no byte was ready at the controller */
    KBD_CHAR,        /* *out is a printable ASCII char or '\n' / '\b' / '\t' */
    KBD_SPECIAL,     /* *out is a KBD_KEY_* code (arrows, modifiers, etc.) */
    KBD_UNKNOWN,     /* *out is the raw scancode; the driver has no name for it */
};

enum kbd_key {
    KBD_KEY_UP = 1,
    KBD_KEY_DOWN,
    KBD_KEY_LEFT,
    KBD_KEY_RIGHT,
    KBD_KEY_HOME,
    KBD_KEY_END,
    KBD_KEY_PGUP,
    KBD_KEY_PGDN,
    KBD_KEY_INSERT,
    KBD_KEY_DELETE,
    KBD_KEY_ESC,
    KBD_KEY_F1,      /* F1..F12 are consecutive: F1 + n */
    KBD_KEY_LSHIFT,
    KBD_KEY_RSHIFT,
    KBD_KEY_LCTRL,
    KBD_KEY_RCTRL,
    KBD_KEY_LALT,
    KBD_KEY_RALT,
    KBD_KEY_CAPSLOCK,
    KBD_KEY_NUMLOCK,
    KBD_KEY_SCROLLLOCK,
    KBD_KEY_PRINTSCREEN,
    KBD_KEY_PAUSE,
};

bool kbd_init(void);
enum kbd_event kbd_poll(char *out);
char kbd_getchar(void);
enum kbd_event kbd_wait(char *out);

#endif
