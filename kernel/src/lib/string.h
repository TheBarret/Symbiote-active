#ifndef LIB_STRING_H
#define LIB_STRING_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strcpy(char *dest, const char *src);
char *strncpy(char *dest, const char *src, size_t n);
char *strchr(const char *s, int c);

/*  additions  */

char *strrchr(const char *s, int c);
char *strcat(char *dest, const char *src);
char *strncat(char *dest, const char *src, size_t n);
size_t strspn(const char *s, const char *accept);
size_t strcspn(const char *s, const char *reject);
char *strpbrk(const char *s, const char *accept);
char *strstr(const char *haystack, const char *needle);

/*  conversions  */

/* Parse an unsigned integer from s, in base `base` (0 = auto-detect from
 * prefix, 10 = decimal, 16 = hex). Skips leading whitespace. If endp is
 * non-NULL, *endp is set to the first unconsumed character.
 * On overflow, returns UINT64_MAX and sets errno-free `*endp` to s. */
uint64_t strtoull(const char *s, char **endp, int base);

/* Same, signed. Accepts a leading '+' or '-'. */
int64_t strtoll(const char *s, char **endp, int base);

/* Strict unsigned parse for kernel callers: consumes the entire string,
 * rejects leading/trailing whitespace, sign characters, and overflow.
 * Returns true on success and writes the value to *out.
 * On failure returns false and leaves *out untouched.
 * The accepted syntax is: decimal digits, or 0x/0X followed by hex digits. */
bool kstrtoull(const char *s, uint64_t *out);

#endif
