#ifndef LIB_MEM_H
#define LIB_MEM_H


#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

/* The compiler is allowed to emit calls to these four even if you never wrote one,
   so they MUST exist and MUST behave exactly like the C standard says. */
void *memcpy(void *restrict dest, const void *restrict src, size_t n);
void *memset(void *s, int c, size_t n);
void *memmove(void *dest, const void *src, size_t n);
int memcmp(const void *a, const void *b, size_t n);

/* Route normal uses through the builtins so the compiler can inline them;
   mem.c holds the real implementations it falls back to. */
#define memcpy  __builtin_memcpy
#define memset  __builtin_memset
#define memmove __builtin_memmove
#define memcmp  __builtin_memcmp

/*  additions  */

/* Find the first byte equal to c in [s, s+n), or NULL. */
void *memchr(const void *s, int c, size_t n);

/* Zero n bytes at p in a way the compiler is not allowed to remove.
 * Use for anything that should not survive being freed. */
void explicit_bzero(void *p, size_t n);

/*  small integer helpers  */

static inline bool is_power_of_two(size_t x) {
    return x != 0 && (x & (x - 1)) == 0;
}

/* Round up to the next multiple of `align`, which must be a power of two. */
static inline size_t align_up(size_t x, size_t align) {
    return (x + align - 1) & ~(align - 1);
}

/* Round down to the previous multiple of `align`, which must be a power of two. */
static inline size_t align_down(size_t x, size_t align) {
    return x & ~(align - 1);
}

/* Pointers, for the common case of aligning a physical or virtual address. */
static inline uintptr_t ptr_align_up(uintptr_t x, size_t align) {
    return (x + align - 1) & ~(uintptr_t)(align - 1);
}
static inline uintptr_t ptr_align_down(uintptr_t x, size_t align) {
    return x & ~(uintptr_t)(align - 1);
}

/*  bit helpers, over a uint64_t  */

static inline bool bit_test(uint64_t word, unsigned bit) {
    return (word >> bit) & 1u;
}
static inline uint64_t bit_set(uint64_t word, unsigned bit) {
    return word | (1ull << bit);
}
static inline uint64_t bit_clear(uint64_t word, unsigned bit) {
    return word & ~(1ull << bit);
}

#endif
