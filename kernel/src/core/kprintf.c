#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <core/console.h>
#include <core/kprintf.h>
#include <lib/string.h>

/* The formatter produces characters one at a time into an "emit" callback,
 * so the same code feeds both the consoles and string buffers. */
typedef void (*emit_fn)(void *ctx, char c);

/*  Emit helpers  */

static void emit_repeat(emit_fn emit, void *ctx, char c, size_t n) {
    for (size_t i = 0; i < n; i++)
        emit(ctx, c);
}

/* Field layout:
 *
 *   [leading space pad] [prefix] [leading zero pad] [digits pad] [body] [trailing space pad]
 *
 * The digits pad is what precision produces (a minimum digit count),
 * and it goes after the prefix so that "%.5x" of 0x1f is "0001f",  and "-%.3d" of -7 is "-007".
 * `numeric` distinguishes the cases where the digits pad applies from %s and %c, where it does not.
 *
 * `zero` pads with '0' between the prefix and the digits, at the field's request;
 * `prec` pads with '0' between the prefix and the digits, at the value's request.
 * Only one of them is typically non-zero, but if both are, the digits pad goes first,
 * then the field pad: this matches what printf does for "%08.5x".
 */
struct field {
    const char *prefix;  size_t plen;
    const char *body;    size_t blen;
    size_t width;
    int    prec;         /* -1 means "no precision" */
    bool   numeric;      /* precision is a minimum digit count */
    bool   left;
    bool   zero;
};

static void emit_field(emit_fn emit, void *ctx, const struct field *f) {
    size_t digits_pad = 0;
    if (f->numeric && f->prec >= 0 && (size_t)f->prec > f->blen)
        digits_pad = (size_t)f->prec - f->blen;

    size_t total = f->plen + digits_pad + f->blen;
    size_t field_pad = f->width > total ? f->width - total : 0;

    if (!f->left && !f->zero)
        emit_repeat(emit, ctx, ' ', field_pad);
    for (size_t i = 0; i < f->plen; i++)
        emit(ctx, f->prefix[i]);
    if (!f->left && f->zero)
        emit_repeat(emit, ctx, '0', field_pad);
    emit_repeat(emit, ctx, '0', digits_pad);
    for (size_t i = 0; i < f->blen; i++)
        emit(ctx, f->body[i]);
    if (f->left)
        emit_repeat(emit, ctx, ' ', field_pad);
}

/* Write `v` in `base` into the END of tmp; returns the start of the digits.
 *
 * tmp must be large enough for the widest possible value.
 * For a 64-bit unsigned value:
 * 64 digits in base 2, 22 in base 8, 20 in base 10, 16 in base 16.
 * 65 bytes plus room for a leading NUL is safe. */
static char *utoa_end(unsigned long long v, unsigned base, bool upper, char *end) {
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char *p = end;
    do {
        *--p = digits[v % base];
        v /= base;
    } while (v);
    return p;
}

/*  the formatter  */

void kvformat(kemit_fn emit, void *ctx, const char *fmt, va_list ap) {
    /* tmp is sized for the widest numeric conversion, base 2.
     * Every conversion that uses utoa_end writes into the tail of this buffer. */
    char tmp[72];
    char *const tmp_end = tmp + sizeof tmp;

    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            emit(ctx, *fmt);
            continue;
        }
        fmt++;

        bool left = false, zero = false;
        for (;; fmt++) {
            if (*fmt == '-') left = true;
            else if (*fmt == '0') zero = true;
            else break;
        }

        /* Width: '*' or a decimal number. */
        size_t width = 0;
        if (*fmt == '*') {
            int w = va_arg(ap, int);
            if (w < 0) { left = true; w = -w; }
            width = (size_t)w;
            fmt++;
        } else {
            while (*fmt >= '0' && *fmt <= '9')
                width = width * 10 + (size_t)(*fmt++ - '0');
        }

        /* Precision: '.' followed by '*' or a decimal number. */
        int prec = -1;
        if (*fmt == '.') {
            fmt++;
            if (*fmt == '*') {
                int p = va_arg(ap, int);
                prec = p < 0 ? -1 : p;
                fmt++;
            } else {
                prec = 0;
                while (*fmt >= '0' && *fmt <= '9')
                    prec = prec * 10 + (*fmt++ - '0');
            }
        }

        /* Length modifier. All of l/ll/z mean 64-bit; h means 16-bit. */
        enum { LEN_NONE, LEN_H, LEN_L, LEN_LL, LEN_Z } len = LEN_NONE;
        for (;; fmt++) {
            if (*fmt == 'h') { len = LEN_H; }
            else if (*fmt == 'l') { len = (len == LEN_L) ? LEN_LL : LEN_L; }
            else if (*fmt == 'z') { len = LEN_Z; }
            else break;
        }

        switch (*fmt) {
        case 'c': {
            char c = (char)va_arg(ap, int);
            const struct field f = {
                "", 0, &c, 1, width, -1, false, left, false,
            };
            emit_field(emit, ctx, &f);
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            size_t n = strlen(s);
            if (prec >= 0 && (size_t)prec < n)
                n = (size_t)prec;
            const struct field f = {
                "", 0, s, n, width, -1, false, left, false,
            };
            emit_field(emit, ctx, &f);
            break;
        }
        case 'd':
        case 'i': {
            long long v;
            switch (len) {
            case LEN_LL:
            case LEN_L:
            case LEN_Z: v = va_arg(ap, long long);          break;
            case LEN_H: v = (short)va_arg(ap, int);         break;
            default:    v = va_arg(ap, int);                break;
            }
            bool neg = v < 0;
            unsigned long long u =
                neg ? 0ULL - (unsigned long long)v : (unsigned long long)v;
            char *p = utoa_end(u, 10, false, tmp_end);
            const struct field f = {
                neg ? "-" : "", neg ? 1 : 0,
                p, (size_t)(tmp_end - p),
                width, prec, true, left, zero,
            };
            emit_field(emit, ctx, &f);
            break;
        }
        case 'u':
        case 'o':
        case 'x':
        case 'X':
        case 'b': {
            unsigned long long v;
            switch (len) {
            case LEN_LL:
            case LEN_L:
            case LEN_Z: v = va_arg(ap, unsigned long long); break;
            case LEN_H: v = (unsigned short)va_arg(ap, unsigned int); break;
            default:    v = va_arg(ap, unsigned int);       break;
            }
            unsigned base;
            bool upper = false;
            switch (*fmt) {
            case 'u': base = 10; break;
            case 'o': base =  8; break;
            case 'x': base = 16; break;
            case 'X': base = 16; upper = true; break;
            case 'b': base =  2; break;
            default:  base = 10; break; /* unreachable */
            }
            char *p = utoa_end(v, base, upper, tmp_end);
            const struct field f = {
                "", 0, p, (size_t)(tmp_end - p),
                width, prec, true, left, zero,
            };
            emit_field(emit, ctx, &f);
            break;
        }
        case 'p': {
            uintptr_t v = (uintptr_t)va_arg(ap, void *);
            char *p = utoa_end(v, 16, false, tmp_end);
            const struct field f = {
                "0x", 2, p, (size_t)(tmp_end - p),
                width, prec, true, left, zero,
            };
            emit_field(emit, ctx, &f);
            break;
        }
        case '%':
            emit(ctx, '%');
            break;
        case '\0':
            return;                     /* format string ended after a '%' */
        default:
            /* Unknown conversion: echo it literally so a typo is visible. */
            emit(ctx, '%');
            emit(ctx, *fmt);
            break;
        }
    }
}

/*  sink 1: the consoles (buffered, flushed in chunks)  */

struct con_sink {
    char buf[128];
    size_t len;
};

static void con_flush(struct con_sink *s) {
    if (s->len) {
        console_write(s->buf, s->len);
        s->len = 0;
    }
}

static void con_emit(void *ctx, char c) {
    struct con_sink *s = ctx;
    s->buf[s->len++] = c;
    if (s->len == sizeof s->buf)
        con_flush(s);
}

void kvprintf(const char *fmt, va_list ap) {
    struct con_sink s = { .len = 0 };
    kvformat(con_emit, &s, fmt, ap);
    con_flush(&s);
}

void kprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
}

/*  sink 2: a caller-supplied string buffer  */

struct str_sink {
    char *buf;
    size_t cap;
    size_t len;     /* counts every character, even ones that did not fit */
};

static void str_emit(void *ctx, char c) {
    struct str_sink *s = ctx;
    /* Leave room for the terminating NUL:
     * write only when the character and the NUL both fit.
     * s->len still advances so the return value is the would-be length, matching snprintf. */
    if (s->cap && s->len + 1 < s->cap)
        s->buf[s->len] = c;
    s->len++;
}

int kvsnprintf(char *buf, size_t cap, const char *fmt, va_list ap) {
    struct str_sink s = { buf, cap, 0 };
    kvformat(str_emit, &s, fmt, ap);
    if (cap)
        buf[s.len < cap ? s.len : cap - 1] = '\0';
    /* s.len can exceed INT_MAX for pathological inputs;
     * clamp so the return value stays a valid int.
     * A caller that cares about truncation checks "return >= cap", not the exact value. */
    return s.len > (size_t)INT32_MAX ? INT32_MAX : (int)s.len;
}

int ksnprintf(char *buf, size_t cap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = kvsnprintf(buf, cap, fmt, ap);
    va_end(ap);
    return n;
}
