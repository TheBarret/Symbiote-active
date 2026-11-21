#ifndef CORE_KLOG_H
#define CORE_KLOG_H
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "core/version.h"

/* codebase: 0.1.2 */

//REF: core/version.h for defined KLOG templates


/* klog */
void klog_init(bool state);
void klog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void klog_ext(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void klog_info(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void klog_warning(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void klog_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void klog_serv(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#endif
