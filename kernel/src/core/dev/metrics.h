#ifndef CORE_METRICS_H
#define CORE_METRICS_H

#include <stddef.h>
#include <stdint.h>
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

#endif /* CORE_METRICS_H */
