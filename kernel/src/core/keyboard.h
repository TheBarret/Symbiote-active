#ifndef CORE_KEYBOARD_H
#define CORE_KEYBOARD_H

#include <stdbool.h>

/* Interrupt-driven PS/2 keyboard driver.
 *
 * The IRQ1 handler only reads the controller and pushes raw scancode bytes into a small ring buffer (it must not print or allocate).
 * kbd_poll() pops bytes from that ring and decodes them (shift, caps lock, 0xE0 prefix) in normal context,
 * so the decoder never runs inside an interrupt.
 * kbd_getchar() sleeps with hlt until a key arrives; it no longer spins.
 * The driver makes no assumption that a controller is present.
 * Call kbd_init() first and check its return value before using the rest. */

/* Event kinds produced by kbd_poll(). */
enum kbd_event {
    KBD_NONE = 0,   /* no byte was ready at the controller */
    KBD_CHAR,       /* *out is a printable ASCII char or '\n' / '\b' / '\t' */
    KBD_SPECIAL,    /* *out is a KBD_KEY_* code (arrows, modifiers, etc.) */
    KBD_UNKNOWN,    /* *out is the raw scancode; the driver has no name for it */
};

/* Special key codes returned in *out when kbd_poll() reports KBD_SPECIAL.
 * These are driver-defined values, not PS/2 scancodes;
 * the shell should treat them as opaque names. */
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
    KBD_KEY_F1,     /* F1..F12 are consecutive: F1 + n */
    KBD_KEY_LSHIFT,
    KBD_KEY_RSHIFT,
    KBD_KEY_LCTRL,
    KBD_KEY_RCTRL,
    KBD_KEY_LALT,
    KBD_KEY_RALT,
    KBD_KEY_CAPSLOCK,
};

/* Initialize the controller.
 * Returns true if a PS/2 controller responded to the self-test and
 * was successfully configured to scancode set 1 with scanning enabled.
 * Returns false if no controller is present or the self-test failed;
 * in that case, kbd_poll() and kbd_getchar() will never produce input and should not be used.
 * Safe to call more than once. */
bool kbd_init(void);

/* Poll for one event. Never blocks.
 * On KBD_CHAR or KBD_SPECIAL or KBD_UNKNOWN, *out is set to the value.
 * On KBD_NONE, *out is not written. */
enum kbd_event kbd_poll(char *out);

/* Blocking convenience: spin until a printable character is ready.
 * Special keys and unknown scancodes are silently discarded.
 * Only call this if kbd_init() returned true. */
char kbd_getchar(void);

/* Block until a decodable event arrives. Never spins, never misses one.
 * Returns the event; *out is set the same way kbd_poll sets it. */
enum kbd_event kbd_wait(char *out);

#endif
