#ifndef CORE_KPRINTF_H
#define CORE_KPRINTF_H

#include <stdarg.h>
#include <stddef.h>
#include <stdbool.h>

/* codebase: 0.1.2 */

/* Formatted output to every console.
 *
 * Supported conversions:
 *   %c %s %d %i %u %x %X %o %b %p %%
 *
 * Flags:
 *   -     left-justify within the field width
 *   0     zero-pad the field (ignored if '-' is also given, or if %s / %c)
 *
 * Width & Precision:
 *   Supports decimal numbers or '*' for dynamic argument fetching.
 *   Protected against INT_MIN overflow and arithmetic overflows.
 *
 * Length modifiers:
 *   l, ll, z   treated as 64-bit
 *   h          truncated to 16 bits
 */

void kprintf(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));
void kvprintf(const char *fmt, va_list ap);

int ksnprintf(char *buf, size_t cap, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
int kvsnprintf(char *buf, size_t cap, const char *fmt, va_list ap);

typedef void (*kemit_fn)(void *ctx, char c);
void kvformat(kemit_fn emit, void *ctx, const char *fmt, va_list ap);

#endif
