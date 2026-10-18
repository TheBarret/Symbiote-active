#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <lib/string.h>

size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n] != '\0')
        n++;
    return n;
}

/* Returns <0, 0, >0 like the standard. */
int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    while (n && *a && *a == *b) {
        a++;
        b++;
        n--;
    }
    if (n == 0)
        return 0;
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

char *strcpy(char *dest, const char *src) {
    char *d = dest;
    while ((*d++ = *src++) != '\0')
        ;
    return dest;
}

char *strncpy(char *dest, const char *src, size_t n) {
    size_t i;
    for (i = 0; i < n && src[i] != '\0'; i++)
        dest[i] = src[i];
    for (; i < n; i++)
        dest[i] = '\0';
    return dest;
}

char *strchr(const char *s, int c) {
    while (*s != (char)c) {
        if (*s == '\0')
            return NULL;
        s++;
    }
    return (char *)s;
}

char *strrchr(const char *s, int c) {
    const char *last = NULL;
    for (;; s++) {
        if (*s == (char)c)
            last = s;
        if (*s == '\0')
            return (char *)last;
    }
}

char *strcat(char *dest, const char *src) {
    char *d = dest + strlen(dest);
    while ((*d++ = *src++) != '\0')
        ;
    return dest;
}

char *strncat(char *dest, const char *src, size_t n) {
    char *d = dest + strlen(dest);
    while (n && *src) {
        *d++ = *src++;
        n--;
    }
    *d = '\0';
    return dest;
}

size_t strspn(const char *s, const char *accept) {
    size_t n = 0;
    while (s[n]) {
        const char *a = accept;
        while (*a && *a != s[n])
            a++;
        if (!*a)
            break;
        n++;
    }
    return n;
}

size_t strcspn(const char *s, const char *reject) {
    size_t n = 0;
    while (s[n]) {
        const char *r = reject;
        while (*r && *r != s[n])
            r++;
        if (*r)
            break;
        n++;
    }
    return n;
}

char *strpbrk(const char *s, const char *accept) {
    while (*s) {
        const char *a = accept;
        while (*a && *a != *s)
            a++;
        if (*a)
            return (char *)s;
        s++;
    }
    return NULL;
}

char *strstr(const char *haystack, const char *needle) {
    /* Empty needle: the standard returns haystack. */
    if (!*needle)
        return (char *)haystack;
    for (; *haystack; haystack++) {
        const char *h = haystack;
        const char *n = needle;
        while (*h && *n && *h == *n) {
            h++;
            n++;
        }
        if (!*n)
            return (char *)haystack;
    }
    return NULL;
}

/*  conversions  */

static int digit_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

uint64_t strtoull(const char *s, char **endp, int base) {
    const char *start = s;

    while (*s == ' ' || *s == '\t' || *s == '\n' ||
           *s == '\r' || *s == '\v' || *s == '\f')
        s++;

    if (base == 0) {
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
            base = 16;
            s += 2;
        } else if (s[0] == '0') {
            base = 8;
            s += 1;
        } else {
            base = 10;
        }
    } else if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
    }

    uint64_t v = 0;
    const char *last = s;
    for (;; s++) {
        int d = digit_value(*s);
        if (d < 0 || d >= base)
            break;
        if (v > (UINT64_MAX - (uint64_t)d) / (uint64_t)base) {
            if (endp) *endp = (char *)start;
            return UINT64_MAX;
        }
        v = v * (uint64_t)base + (uint64_t)d;
        last = s + 1;
    }

    if (endp)
        *endp = (char *)last;
    return v;
}

int64_t strtoll(const char *s, char **endp, int base) {
    const char *start = s;

    while (*s == ' ' || *s == '\t' || *s == '\n' ||
           *s == '\r' || *s == '\v' || *s == '\f')
        s++;

    int neg = 0;
    if (*s == '+') {
        s++;
    } else if (*s == '-') {
        neg = 1;
        s++;
    }

    char *inner_end = NULL;
    uint64_t mag = strtoull(s, &inner_end, base);

    /* The magnitude limit differs by sign: INT64_MAX for positive,
     * (uint64_t)INT64_MAX + 1 for negative (to allow INT64_MIN, whose
     * magnitude is one more than INT64_MAX). Written as two branches
     * so the boundary is visible. */
    uint64_t limit = neg ? (uint64_t)INT64_MAX + 1 : (uint64_t)INT64_MAX;
    if (mag > limit) {
        if (endp) *endp = (char *)start;
        return neg ? INT64_MIN : INT64_MAX;
    }

    if (endp)
        *endp = inner_end;
    return neg ? -(int64_t)mag : (int64_t)mag;
}

/* strtoull */

bool kstrtoull(const char *s, uint64_t *out) {
    if (s == NULL || *s == '\0')
        return false;

    unsigned base = 10;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s += 2;
        if (*s == '\0')
            return false;
    }

    uint64_t v = 0;
    for (; *s; s++) {
        int d = digit_value(*s);
        if (d < 0 || (unsigned)d >= base)
            return false;
        if (v > (UINT64_MAX - (uint64_t)d) / base)
            return false;
        v = v * base + (uint64_t)d;
    }
    *out = v;
    return true;
}
