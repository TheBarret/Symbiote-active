#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <lib/string.h>
#include <core/arch/heap.h>
#include <core/dev/timer.h>
#include <core/dev/vfs.h>
#include <core/dev/env.h>
#include <core/mask.h>
#include <core/klog.h>
#include <core/bus.h>
#include <core/panic.h>
#include <core/dev/metrics.h>

/* Metric configuration and state */
struct metrics_device {
    uint64_t interval_ms;
    int      timer_id;
    bool     initialized;
};

static struct metrics_device *g_metrics = NULL;

/* Helper to write a numeric string value to a VFS file path */
static bool metrics_write_file(const char *path, uint64_t val) {
    struct vfs_file *f = vfs_open(path, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC);
    if (f == NULL) return false;

    char buf[32];
    int len = 0;
    uint64_t temp = val;

    if (temp == 0) {
        buf[0] = '0';
        len = 1;
    } else {
        char rev[32];
        int r_len = 0;
        while (temp > 0) {
            rev[r_len++] = '0' + (char)(temp % 10);
            temp /= 10;
        }
        for (int i = 0; i < r_len; i++) {
            buf[i] = rev[r_len - 1 - i];
        }
        len = r_len;
    }
    buf[len] = '\0';

    long written = vfs_write(f, buf, (size_t)len);
    vfs_close(f);
    return written == (long)len;
}

/* Core metric sampling and update cycle */
static int metrics_sample_and_update(void) {
    // Stub / integration points for actual kernel memory & interrupt counters
    // In a full integration, these query pmm_get_free_frames(), pmm_get_used_frames(), pic status, etc.
    uint64_t mem_free = 1024 * 1024 * 64;  // Example: 64 MB free
    uint64_t mem_used = 1024 * 1024 * 16;  // Example: 16 MB used
    uint64_t irq_free = 12;                // Example free interrupt lines
    uint64_t irq_used = 4;                 // Example active interrupt lines

    bool success = true;
    success &= metrics_write_file("/metrics/memory/free", mem_free);
    success &= metrics_write_file("/metrics/memory/used", mem_used);
    success &= metrics_write_file("/metrics/interrupts/free", irq_free);
    success &= metrics_write_file("/metrics/interrupts/used", irq_used);

    return success ? 0 : -1;
}

/* Timer callback for low-interval background updates */
static void metrics_timer_cb(void *arg) {
    struct metrics_device *m = arg;
    if (m == NULL || !m->initialized) return;

    if (metrics_sample_and_update() != 0) {
        klog_warning("metrics: background write logic failed\n");
    }

    const struct timer_ops *timer = bus_timer();
    if (timer != NULL) {
        m->timer_id = timer->after(m->interval_ms, metrics_timer_cb, m);
    }
}

/* Register routing symbols in the environment store (env.c) once at boot */
static void metrics_register_env_paths(void) {
    const struct env_ops *env = bus_env();
    if (env == NULL) {
        klog_warning("metrics: env bus service unavailable during init\n");
        return;
    }
    // owned by BUS_METRICS (config.h)
    uint32_t mask = env_mask_build(BUS_METRICS, ENV_TYPE_PATH, ENV_FLAG_PINNED | ENV_FLAG_READONLY, 0);

    // Set up path routing symbols pointing to the VFS nodes
    env->set("kernel.metrics", "memory_free",    "/metrics/memory/free",    0, 0, 10, mask);
    env->set("kernel.metrics", "memory_used",    "/metrics/memory/used",    0, 0, 10, mask);
    env->set("kernel.metrics", "interrupt_free", "/metrics/interrupts/free", 0, 0, 10, mask);
    env->set("kernel.metrics", "interrupt_used", "/metrics/interrupts/used", 0, 0, 10, mask);
}

static int metrics_bus_update(void) {
    return metrics_sample_and_update();
}

static const struct metrics_ops metrics_bus_ops = {
    .update = metrics_bus_update,
};

void metrics_init(uint64_t interval_ms) {
    if (g_metrics != NULL) {
        klog_warning("metrics_init() called twice; ignoring\n");
        return;
    }

    struct metrics_device *m = kzalloc(sizeof(*m));
    if (m == NULL) PANIC("metrics: failed to allocate device state");

    m->interval_ms = interval_ms > 0 ? interval_ms : 1000; // Default to 1 second
    m->timer_id = -1;
    m->initialized = true;
    g_metrics = m;

    // create VFS directory structure
    vfs_mkdir("/metrics");
    vfs_mkdir("/metrics/memory");
    vfs_mkdir("/metrics/interrupts");

    // initial sample to populate files
    metrics_sample_and_update();

    // register discovery paths into environment store (env.c)
    metrics_register_env_paths();

    // schedule recurring background updates via timer
    const struct timer_ops *timer = bus_timer();
    if (timer != NULL) {
        m->timer_id = timer->after(m->interval_ms, metrics_timer_cb, m);
        if (m->timer_id < 0) {
            klog_warning("metrics: timer scheduling failed; metrics will be static\n");
        }
    } else {
        klog_warning("metrics: timer service not found; background refresh disabled\n");
    }
    // announce exposure
    klog("→ metrics_init(interval=%lums)\n", m->interval_ms);

    // register on the static software bus
    struct bus_device dev = {
        .name         = "metrics",
        .class        = BUS_CLASS_METRIC,
        .version      = 1,
        .ops          = &metrics_bus_ops,
        .private_data = m,
        .owner_id     = BUS_METRICS,
    };
    if (bus_register(&dev) != 0) {
        klog_warning("metrics: bus registration failed\n");
    } else {
        bus_test("metrics");
    }
}

void metrics_destroy(void) {
    if (g_metrics == NULL) return;

    const struct timer_ops *timer = bus_timer();
    if (timer != NULL && g_metrics->timer_id >= 0) {
        timer->cancel(g_metrics->timer_id);
    }

    kfree(g_metrics);
    g_metrics = NULL;
}
