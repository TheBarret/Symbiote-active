#ifndef CORE_SERVICES_LOGGER_H
#define CORE_SERVICES_LOGGER_H

#include <stdarg.h>
#include <stddef.h>
#include <core/services/addr.h>
#include <core/services/router.h>

#define LOGGER_MAX_SIZE 256u

bool log_open(const char *path);
bool log_close(void);
int log_write(const char *fmt, ...);

#endif
