#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <core/arch/cpu.h>
#include <core/klog.h>
#include <core/kprintf.h>

/* codebase: 0.1.2 */

/* Logging + rdtsc helpers  */

static uint64_t klog_last_tsc;
static uint64_t klog_tsc_hz;
static bool     klog_tsc_ready;
static bool     klog_tsc_enabled;

// init
void klog_init(bool state) {
    klog_tsc_enabled = state;
    klog_tsc_hz = cpu_tsc_hz();
    klog_tsc_ready = true;
    klog_last_tsc = rdtsc();    /* first line reads [+0] */
    if (klog_tsc_hz) {
        kprintf("klog_init(): TSC Found, %llu MHz\n", (unsigned long long)(klog_tsc_hz / 1000000));
    } else {
        klog_tsc_hz = 2500000000ull;    /* 2.5 GHz fallback, best-effort */
#ifdef SYM_MEMDEBUG
        kprintf("klog_init(): TSC not found (fallback: 2.5 Ghz)\n");
#endif
    }
}
static void klog_emit(const char *tag, const char *fmt, va_list ap) {
    if (klog_tsc_enabled) {
        uint64_t now = rdtsc();
        uint64_t delta = now - klog_last_tsc;
        klog_last_tsc = now;

        char stamp[24];
        if (klog_tsc_hz) {
            /* Use __uint128_t if available so (delta * 1e6) doesn't overflow on long idle gaps */
            uint64_t us = (uint64_t)(((__uint128_t)delta * 1000000ull) / klog_tsc_hz);
            ksnprintf(stamp, sizeof(stamp), "[+%llu.%03llu]",
                      (unsigned long long)(us / 1000ull),
                      (unsigned long long)(us % 1000ull));
        } else {
            ksnprintf(stamp, sizeof(stamp), "[+%llu]", (unsigned long long)delta);
        }
        stamp[sizeof(stamp) - 1] = '\0';

        /* Fixed 14-char column + 1 separator space */
        kprintf("%-14.14s ", stamp);
    } else {
        kprintf("[◙] ");
    }

    kprintf("%s ", tag);
    kvprintf(fmt, ap);
}

void klog(const char *fmt, ...)          { va_list ap; va_start(ap, fmt); klog_emit(SYM_TAG, fmt, ap);         va_end(ap); }
void klog_ext(const char *fmt, ...)      { va_list ap; va_start(ap, fmt); klog_emit(SYM_TAG_EXT, fmt, ap);     va_end(ap); }
void klog_info(const char *fmt, ...)     { va_list ap; va_start(ap, fmt); klog_emit(SYM_TAG_INFO, fmt, ap);    va_end(ap); }
void klog_warning(const char *fmt, ...)  { va_list ap; va_start(ap, fmt); klog_emit(SYM_TAG_WARNING, fmt, ap); va_end(ap); }
void klog_error(const char *fmt, ...)    { va_list ap; va_start(ap, fmt); klog_emit(SYM_TAG_ERROR, fmt, ap);   va_end(ap); }
void klog_serv(const char *fmt, ...)    { va_list ap; va_start(ap, fmt); klog_emit(SYM_TAG_SERV, fmt, ap);   va_end(ap); }
