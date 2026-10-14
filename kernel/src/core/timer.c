#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <core/version.h>
#include <core/cpu.h>
#include <core/isr.h>
#include <core/panic.h>
#include <core/timer.h>
#include <core/kprintf.h>
#include <core/klog.h>
#include <core/bus.h>

// Moved to ref: core/version.h
//#define PIT_FREQ  1193182ull        /* Hz, the PIT's input clock */
//#define PIT_CH0   0x40
//#define PIT_CMD   0x43

static volatile uint64_t ticks;
static uint64_t divisor = 1;
static bool inited;

/* bus back-end */

struct timer_task {
    uint64_t deadline_ticks;   /* fire when ticks >= */
    void   (*cb)(void *);
    void    *arg;
    int      id;
    bool     active;
    bool     ready;
};

static struct timer_task tasks[TIMER_MAX_TASKS];
static int next_task_id = 1;   /* 0 means "no id"; wraps are ignored */
static bool in_drain = false;

/* Forward declaration; defined at the bottom after the functions it points at. */
static const struct timer_ops timer_bus_ops;

/* Runs in IRQ context: count and nothing else. */
static void timer_irq(void *ctx) {
    (void)ctx;
    ticks++;
    for (size_t i = 0; i < TIMER_MAX_TASKS; i++) {
        if (tasks[i].active && !tasks[i].ready &&
            tasks[i].deadline_ticks <= ticks) {
            tasks[i].ready = true;
        }
    }
}

void timer_init(uint32_t hz) {
    if (hz == 0)
        hz = TIMER_HZ;
    uint64_t d = PIT_FREQ / hz;
    if (d < 1)     d = 1;
    if (d > 65535) d = 65535;       /* 16-bit counter */
    divisor = d;

    outb(PIT_CMD, 0x34);            /* channel 0, lobyte/hibyte, mode 2 (rate generator) */
    outb(PIT_CH0, (uint8_t)(d & 0xFF));
    outb(PIT_CH0, (uint8_t)(d >> 8));

    if (irq_register(0, timer_irq, NULL) != 0)
        PANIC("timer: IRQ0 already taken");
    inited = true;
    klog("→ timer_init() 16-bit counter, channel 0, lobyte/hibyte, mode 2 (rate generator)\n");

    struct bus_device dev = {
        .name         = "timer",
        .class        = BUS_CLASS_TIMER,
        .version      = 1,
        .ops          = &timer_bus_ops,
        .private_data = NULL,
    };
    bus_register(&dev);
    bus_test("timer");
}

uint32_t timer_hz(void) {
    return (uint32_t)((PIT_FREQ + divisor / 2) / divisor);
}

uint64_t timer_ticks(void) {
    return ticks;
}

uint64_t uptime_ms(void) {
    return ticks * divisor * 1000ull / PIT_FREQ;
}

/* ms -> ticks, rounded up. Same rounding timer_sleep_ms uses. */
static uint64_t ms_to_ticks(uint64_t ms) {
    return (ms * PIT_FREQ + divisor * 1000ull - 1) / (divisor * 1000ull);
}

void timer_sleep_ms(uint64_t ms) {
    if (!inited || ms == 0)
        return;
    if (ms > 1000000000000ull)      /* keep the arithmetic below from overflowing */
        ms = 1000000000000ull;

    uint64_t need = (ms * PIT_FREQ + divisor * 1000ull - 1) / (divisor * 1000ull);   /* round up */

    uint64_t flags = cpu_irq_save();
    uint64_t target = ticks + need;
    while (ticks < target) {
        cpu_sti_hlt();
        cpu_cli();
    }
    cpu_irq_restore(flags);
}

int timer_after(uint64_t ms, void (*cb)(void *), void *arg) {
    if (cb == NULL)
        return -1;

    uint64_t flags = cpu_irq_save();
    int result = -1;

    for (size_t i = 0; i < TIMER_MAX_TASKS; i++) {
        if (tasks[i].id != 0)
            continue;

        int id = next_task_id++;
        if (next_task_id <= 0)
            next_task_id = 1;   /* wrap; stale ids simply never match */

        tasks[i].deadline_ticks = ticks + ms_to_ticks(ms);
        tasks[i].cb             = cb;
        tasks[i].arg            = arg;
        tasks[i].id             = id;
        tasks[i].active         = true;
        tasks[i].ready          = false;
        result = id;
        break;
    }

    cpu_irq_restore(flags);
    return result;
}

int timer_cancel(int id) {
    if (id <= 0)
        return -1;

    uint64_t flags = cpu_irq_save();
    int result = -1;

    for (size_t i = 0; i < TIMER_MAX_TASKS; i++) {
        if (tasks[i].id == id && tasks[i].active) {
            tasks[i].id     = 0;
            tasks[i].active = false;
            tasks[i].ready  = false;
            tasks[i].cb     = NULL;
            tasks[i].arg    = NULL;
            result = 0;
            break;
        }
    }

    cpu_irq_restore(flags);
    return result;
}

void timer_drain(void) {
    if (in_drain)
        return;   /* not reentrant */

    in_drain = true;

    for (size_t i = 0; i < TIMER_MAX_TASKS; i++) {
        if (!tasks[i].active || !tasks[i].ready)
            continue;

        /* Snapshot and clear before calling: the callback may schedule
         * a new task and must be able to reuse this slot. */
        void (*cb)(void *) = tasks[i].cb;
        void  *arg         = tasks[i].arg;

        uint64_t flags = cpu_irq_save();
        tasks[i].id     = 0;
        tasks[i].active = false;
        tasks[i].ready  = false;
        tasks[i].cb     = NULL;
        tasks[i].arg    = NULL;
        cpu_irq_restore(flags);

        cb(arg);
    }

    in_drain = false;
}

/* Defined last so it can reference the functions above without forward-declaring each of them.
 * The forward declaration near the top of the file lets timer_init take its address. */
static const struct timer_ops timer_bus_ops = {
    .ticks     = timer_ticks,
    .uptime_ms = uptime_ms,
    .hz        = timer_hz,
    .sleep_ms  = timer_sleep_ms,
    .after     = timer_after,
    .cancel    = timer_cancel,
};
