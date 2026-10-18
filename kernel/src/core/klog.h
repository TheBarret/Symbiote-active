#ifndef CORE_KLOG_H
#define CORE_KLOG_H

#include "core/version.h"

//Ref: core/version.h for settings


/* klog */
void klog_init(void);
void klog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void klog_ext(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void klog_info(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void klog_warning(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void klog_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void klog_bus(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void klog_env(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
