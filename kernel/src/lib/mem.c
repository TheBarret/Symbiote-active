#include <stddef.h>
#include <stdint.h>
#include <lib/mem.h>

/* Do NOT rename or remove these!
 *  See mem.h. The macros from mem.h are undone here so we define the real functions, not the builtins.
 */
#undef memcpy
#undef memset
#undef memmove
#undef memcmp

void *memcpy(void *restrict dest, const void *restrict src, size_t n) {
    uint8_t *d = dest;
    const uint8_t *s = src;
    for (size_t i = 0; i < n; i++)
        d[i] = s[i];
    return dest;
}

void *memset(void *s, int c, size_t n) {
    uint8_t *p = s;
    for (size_t i = 0; i < n; i++)
        p[i] = (uint8_t)c;
    return s;
}

/* Overlap-safe: copy forwards if dest is below src, backwards otherwise. */
void *memmove(void *dest, const void *src, size_t n) {
    uint8_t *d = dest;
    const uint8_t *s = src;
    if (d < s) {
        for (size_t i = 0; i < n; i++)
            d[i] = s[i];
    } else if (d > s) {
        for (size_t i = n; i > 0; i--)
            d[i - 1] = s[i - 1];
    }
    return dest;
}

int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *x = a, *y = b;
    for (size_t i = 0; i < n; i++) {
        if (x[i] != y[i])
            return x[i] < y[i] ? -1 : 1;
    }
    return 0;
}

void *memchr(const void *s, int c, size_t n) {
    const uint8_t *p = s;
    uint8_t b = (uint8_t)c;
    for (size_t i = 0; i < n; i++) {
        if (p[i] == b)
            return (void *)(p + i);
    }
    return NULL;
}

/* The volatile cast is the whole point: without it, the compiler is free
 * to see the stores as dead and remove the call. */
void explicit_bzero(void *p, size_t n) {
    volatile uint8_t *v = p;
    while (n--)
        *v++ = 0;
}
