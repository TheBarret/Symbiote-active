#ifndef CORE_METRICS_H
#define CORE_METRICS_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include <core/version.h>
#include <core/bus.h>

/* Metrics Subsystem
 *
 * Collects low-interval kernel telemetry (free memory, used memory, interrupts free/used)
 * and exposes them via VFS file nodes, with routing symbols registered in the environment store (env.c) using ENV_TYPE_PATH.
 */

struct metrics_ops {
    int (*update)(void); /* Force an immediate telemetry sample and VFS write */
};

/* Bus accessor */
const struct metrics_ops *bus_metrics(void);

/* Lifecycle */
void metrics_init(uint64_t interval_ms);
void metrics_destroy(void);


/* Read a raw u64 from a metric path (e.g., "/metrics/memory/free").
 * Returns 0 if the file doesn't exist, is empty, or fails to parse. */
uint64_t metrics_read_u64(const char *path);

/* Read a raw string from a metric path into buf. Returns false on failure. */
bool metrics_read_str(const char *path, char *buf, size_t cap);

#endif /* CORE_METRICS_H */
