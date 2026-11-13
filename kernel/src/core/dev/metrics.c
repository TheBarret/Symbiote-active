#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <lib/string.h>
#include <core/arch/heap.h>
#include <core/arch/pmm.h>
#include <core/dev/timer.h>
#include <core/dev/vfs.h>
#include <core/dev/env.h>
#include <core/mask.h>
#include <core/klog.h>
#include <core/bus.h>
#include <core/panic.h>
#include <core/kprintf.h>
#include <core/dev/metrics.h>

/* codebase: 0.1.2 */

/* Metric configuration and state */
struct metrics_device {
    uint64_t interval_ms;
    int      timer_id;
    bool     initialized;
};

static struct metrics_device *g_metrics = NULL;

/* The registration table. Each entry is a single endpoint. */
struct metric_source {
    const char *path;
    void (*sample)(char *buf, size_t max);
};


/* SOURCE CONTROL (raw units) */


static void sample_mem_total(char *buf, size_t max) {
    struct pmm_stats pmm = pmm_get_stats();
    ksnprintf(buf, max, "%llu", (unsigned long long)pmm.total_frames);
}

static void sample_mem_free(char *buf, size_t max) {
    struct pmm_stats pmm = pmm_get_stats();
    ksnprintf(buf, max, "%llu", (unsigned long long)pmm.free_frames);
}

static void sample_timer_ticks(char *buf, size_t max) {
    ksnprintf(buf, max, "%llu", (unsigned long long)timer_ticks());
}

static void sample_timer_uptime(char *buf, size_t max) {
    ksnprintf(buf, max, "%llu", (unsigned long long)uptime_ms());
}

static void sample_timer_hz(char *buf, size_t max) {
    ksnprintf(buf, max, "%lu", (unsigned long)timer_hz());
}

static void sample_timer_divisor(char *buf, size_t max) {
    ksnprintf(buf, max, "%llu", (unsigned long long)timer_divisor());
}

static void sample_timer_tasks_active(char *buf, size_t max) {
    ksnprintf(buf, max, "%llu", (unsigned long long)timer_tasks_active());
}

static void sample_timer_tasks_pending(char *buf, size_t max) {
    ksnprintf(buf, max, "%llu", (unsigned long long)timer_tasks_pending());
}

/* METRIC TABLE */


static const struct metric_source metric_table[] = {
    /* Memory */
    { "/metrics/memory/total", sample_mem_total },
    { "/metrics/memory/free",  sample_mem_free  },

    /* Timer */
    { "/metrics/timer/ticks",     sample_timer_ticks  },
    { "/metrics/timer/uptime_ms", sample_timer_uptime },
    { "/metrics/timer/hz",        sample_timer_hz     },
    { "/metrics/timer/divisor",        sample_timer_divisor        },
    { "/metrics/timer/tasks_active",   sample_timer_tasks_active   },
    { "/metrics/timer/tasks_pending",  sample_timer_tasks_pending  },

    /* Sentinel */
    { NULL, NULL }
};

#define METRIC_TABLE_COUNT (sizeof(metric_table) / sizeof(metric_table[0]) - 1)


/* HELPERS */


/* Write a raw string value to a VFS file path */
static bool metrics_write_file_str(const char *path, const char *val) {
    struct vfs_file *f = vfs_open(path, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC);
    if (f == NULL) return false;

    size_t len = strlen(val);
    long written = vfs_write(f, val, len);
    vfs_close(f);
    return written == (long)len;
}

/* Read a raw u64 from a metric path */
uint64_t metrics_read_u64(const char *path) {
    struct vfs_file *f = vfs_open(path, VFS_O_RDONLY);
    if (!f) return 0;

    char buf[32];
    long n = vfs_read(f, buf, sizeof(buf) - 1);
    vfs_close(f);

    if (n <= 0) return 0;
    buf[n] = '\0';

    uint64_t val = 0;
    for (long i = 0; i < n; i++) {
        if (buf[i] < '0' || buf[i] > '9') break;
        val = val * 10 + (uint64_t)(buf[i] - '0');
    }
    return val;
}

/* Read a raw string from a metric path */
bool metrics_read_str(const char *path, char *buf, size_t cap) {
    if (cap == 0) return false;
    struct vfs_file *f = vfs_open(path, VFS_O_RDONLY);
    if (!f) return false;

    long n = vfs_read(f, buf, cap - 1);
    vfs_close(f);

    if (n < 0) return false;
    buf[n] = '\0';
    return true;
}

/* CORE METRIC SAMPLING AND UPDATE CYCLE */

static int metrics_sample_and_update(void) {
    bool success = true;
    char buf[64];

    for (size_t i = 0; i < METRIC_TABLE_COUNT; i++) {
        metric_table[i].sample(buf, sizeof(buf));
        if (!metrics_write_file_str(metric_table[i].path, buf)) {
            success = false;
        }
    }

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

    /* Owner is BUS_METRICS (config.h), no PRIVATE bits for now */
    uint32_t mask = env_mask_build(BUS_METRICS, ENV_TYPE_PATH, ENV_FLAG_PINNED | ENV_FLAG_READONLY, 0);

    for (size_t i = 0; i < METRIC_TABLE_COUNT; i++) {
        /* Derive the env name from the path.
         * "/metrics/memory/free" -> "memory_free" Skip the "/metrics/" prefix, then replace '/' with '_'.
         */
        const char *p = metric_table[i].path;
        if (strncmp(p, "/metrics/", 9) == 0) p += 9;

        char key[64];
        size_t k = 0;
        while (*p && k < sizeof(key) - 1) {
            key[k++] = (*p == '/') ? '_' : *p;
            p++;
        }
        key[k] = '\0';

        env->set("metrics", key, metric_table[i].path, 0, 0, ENV_IMPORTANCE_MAX, mask);
    }
}

static int metrics_bus_update(void) {
    return metrics_sample_and_update();
}

static const struct metrics_ops metrics_bus_ops = {
    .update = metrics_bus_update,
};


/* LIFECYCLE */

void metrics_init(uint64_t interval_ms) {
    if (g_metrics != NULL) {
        klog_warning("metrics_init() called twice; ignoring\n");
        return;
    }

    struct metrics_device *m = kzalloc(sizeof(*m));
    if (m == NULL) PANIC("metrics: failed to allocate device state");

    m->interval_ms = interval_ms > 0 ? interval_ms : 1000;
    m->timer_id = -1;
    m->initialized = true;
    g_metrics = m;

    /* Create VFS directory structure */
    vfs_mkdir("/metrics");
    vfs_mkdir("/metrics/memory");
    vfs_mkdir("/metrics/timer");

    /* Register discovery paths into environment store */
    metrics_register_env_paths();

    /* Initial sample to populate files so `ls` works immediately */
    metrics_sample_and_update();

    /* Schedule recurring background updates via timer */
    const struct timer_ops *timer = bus_timer();
    if (timer != NULL) {
        m->timer_id = timer->after(m->interval_ms, metrics_timer_cb, m);
        if (m->timer_id < 0) {
            klog_warning("metrics: timer scheduling failed; metrics will be static\n");
        }
    } else {
        klog_warning("metrics: timer service not found; background refresh disabled\n");
    }

    klog("→ metrics_init(interval=%lums)\n", m->interval_ms);

    /* Register on the static software bus */
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
