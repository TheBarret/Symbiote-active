#include "klog.h"
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <core/cpu.h>
#include <core/klog.h>
#include <core/kprintf.h>


/* Logging + rdtsc helpers  */

static uint64_t klog_last_tsc;
static uint64_t klog_tsc_hz;
static bool     klog_tsc_ready;

// init
void klog_init(void) {
    klog_tsc_hz = cpu_tsc_hz();
    klog_tsc_ready = true;
    klog_last_tsc = rdtsc();    /* first line reads [+0] */
    if (klog_tsc_hz) {
        kprintf("klog_init(): TSC Found, %llu MHz\n", (unsigned long long)(klog_tsc_hz / 1000000));
    } else {
        klog_tsc_hz = 2500000000ull;    /* 2.5 GHz fallback, best-effort */
        kprintf("klog_init(): TSC not found (fallback: 2.5 Ghz)\n");
    }
}

static void klog_emit(const char *tag, const char *fmt, va_list ap) {
    uint64_t now = rdtsc();
    uint64_t delta = now - klog_last_tsc;
    klog_last_tsc = now;

    char stamp[24];
    if (klog_tsc_hz) {
        uint64_t us = delta * 1000000ull / klog_tsc_hz;
        ksnprintf(stamp, sizeof stamp, "[+%llu.%03llu]",
                  (unsigned long long)(us / 1000),
                  (unsigned long long)(us % 1000));
    } else {
        ksnprintf(stamp, sizeof stamp, "[+%llu]", (unsigned long long)delta);
    }

    /* padding */
    kprintf("%-10.10s ", stamp);

    kprintf("%s ", tag);
    kvprintf(fmt, ap);
}

void klog(const char *fmt, ...)          { va_list ap; va_start(ap, fmt); klog_emit(SYM_TAG, fmt, ap);         va_end(ap); }
void klog_ext(const char *fmt, ...)      { va_list ap; va_start(ap, fmt); klog_emit(SYM_TAG_EXT, fmt, ap);     va_end(ap); }
void klog_info(const char *fmt, ...)     { va_list ap; va_start(ap, fmt); klog_emit(SYM_TAG_INFO, fmt, ap);    va_end(ap); }
void klog_warning(const char *fmt, ...)  { va_list ap; va_start(ap, fmt); klog_emit(SYM_TAG_WARNING, fmt, ap); va_end(ap); }
void klog_error(const char *fmt, ...)    { va_list ap; va_start(ap, fmt); klog_emit(SYM_TAG_ERROR, fmt, ap);   va_end(ap); }
void klog_bus(const char *fmt, ...)    { va_list ap; va_start(ap, fmt); klog_emit(SYM_TAG_BUS, fmt, ap);   va_end(ap); }
void klog_env(const char *fmt, ...)    { va_list ap; va_start(ap, fmt); klog_emit(SYM_TAG_ENV, fmt, ap);   va_end(ap); }
