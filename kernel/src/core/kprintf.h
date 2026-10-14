#ifndef CORE_KPRINTF_H
#define CORE_KPRINTF_H

#include <stdarg.h>
#include <stddef.h>
#include <stdbool.h>

/* Formatted output to every console.
 *
 * Supported conversions:
 *   %c %s %d %i %u %x %X %o %b %p %%
 *
 * Flags:
 *   -    left-justify within the field width
 *   0    zero-pad the field (ignored if '-' is also given, or if the conversion is %s or %c, matching the standard)
 *
 * Width:
 *   a decimal number, or '*' to read it from the argument list
 *
 * Precision:
 *   "." followed by a decimal number, or ".*" to read it from the argument list.
 *   For %s, the precision limits the maximum number of characters read from the string.
 *   For numeric conversions, it is the minimum number of digits, zero-padded on the left of the digits.
 *   A negative precision read from '*' is treated as "no precision".
 *
 * Length modifiers:
 *   l, ll, z   all treated as 64-bit, which is what the x86_64 ABI does
 *   h          truncates to 16 bits (short / unsigned short)
 *
 * Any other conversion character is printed literally, prefixed by the '%' that introduced it.
 * This is intentional: a typo in a format string shows up in the output instead of disappearing or being reinterpreted.
 *
 * The compiler is told these are printf-shaped, so -Wformat diagnoses mismatched arguments at build time.
 *
 * The return values follow snprintf: the number of characters that would have been written,
 * not the number actually written, so a caller can detect truncation with "return >= cap". */

void kprintf(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));
void kvprintf(const char *fmt, va_list ap);

int ksnprintf(char *buf, size_t cap, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
int kvsnprintf(char *buf, size_t cap, const char *fmt, va_list ap);

/* Lowest-level entry point.
 *
 * `emit` is called once per output character, in order.
 * It must not modify anything the caller relies on being stable during the call (the formatter holds no buffer of its own).
 * A sink that accumulates into a fixed buffer, a ring, or a string builder is a few lines. */
typedef void (*kemit_fn)(void *ctx, char c);
void kvformat(kemit_fn emit, void *ctx, const char *fmt, va_list ap);

#endif
