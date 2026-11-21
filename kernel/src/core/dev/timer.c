#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <core/version.h>
#include <core/arch/cpu.h>
#include <core/arch/isr.h>
#include <core/dev/timer.h>
#include <core/panic.h>
#include <core/kprintf.h>
#include <core/klog.h>

/* codebase: 0.2.0 */

static volatile uint64_t ticks;
static uint64_t divisor = 1;
static bool inited;

/* bus back-end */

struct timer_task {
    uint64_t      deadline_ticks;   /* fire when ticks >= */
    void        (*cb)(void *);
    void         *arg;
    int           id;
    bool          active;
    volatile bool ready;            /* set by the ISR, cleared by thread context */
};

static struct timer_task tasks[TIMER_MAX_TASKS];
static int next_task_id = 1;        /* 0 means "no id" */
static bool in_drain = false;

/* Runs in IRQ context: count, and mark due tasks ready. Cost is
 * O(TIMER_MAX_TASKS) per tick. */
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

/* Next task id: always positive, wraps from INT32_MAX back to 1 without
 * signed overflow. */
static int timer_alloc_id(void) {
    int id = next_task_id;
    next_task_id = (next_task_id == INT32_MAX) ? 1 : next_task_id + 1;
    return id;
}

/* Timer: ms -> ticks safe conversions
 *
 *     ticks = f(ms * hz / 1000)
 *
 * where f is the rounding function. The product ms * hz can overflow, so the
 * computation is split:
 *
 *     whole = (ms / 1000) * hz            integer part
 *     rem   = (ms % 1000) * hz            remaining numerator over 1000
 *     ticks = whole + (rem + bias) / 1000
 *
 * bias selects the rounding: 0 = floor, 999 = ceil, 500 = round (ties up).
 *
 * rem is always small (< 1000 * 2^32), so only `whole` can overflow. If the
 * result would not fit, the conversion saturates at UINT64_MAX. Callers treat
 * UINT64_MAX as "unrepresentable".
 *
 * Returns 0 if ms == 0, or if the timer is not initialised (the caller gets
 * "no delay" rather than a conversion at a frequency that is not running).
 */
static uint64_t ms_to_ticks_biased(uint64_t ms, uint64_t bias) {
    if (ms == 0 || !inited)
        return 0;

    uint64_t hz = timer_hz();
    if (hz == 0)
        return 0;

    uint64_t secs = ms / 1000;
    /* result <= secs * hz + hz, so require secs <= UINT64_MAX / hz - 1 */
    if (secs > UINT64_MAX / hz - 1)
        return UINT64_MAX;

    return secs * hz + ((ms % 1000) * hz + bias) / 1000;
}

uint64_t ms_to_ticks_floor(uint64_t ms) { return ms_to_ticks_biased(ms, 0);   }
uint64_t ms_to_ticks_ceil (uint64_t ms) { return ms_to_ticks_biased(ms, 999); }
uint64_t ms_to_ticks_round(uint64_t ms) { return ms_to_ticks_biased(ms, 500); }

void timer_sleep_ms(uint64_t ms) {
    if (in_drain)
        PANIC("timer_sleep_ms from a drain callback");
    if (ms == 0)
        return;
    if (!inited) {
        klog_warning("timer_sleep_ms: timer not initialised, not sleeping\n");
        return;
    }

    uint64_t need = ms_to_ticks_ceil(ms);
    if (need == 0)
        need = 1;   /* a nonzero request sleeps at least one tick */
    if (need == UINT64_MAX || need > UINT64_MAX - ticks) {
        klog_warning("timer_sleep_ms: duration unrepresentable, not sleeping\n");
        return;
    }

    uint64_t flags = cpu_irq_save();
    uint64_t target = ticks + need;
    while (ticks < target) {
        cpu_sti_hlt();
        cpu_cli();
    }
    cpu_irq_restore(flags);
}

int timer_after(uint64_t ms, void (*cb)(void *), void *arg) {
    if (cb == NULL) {
        klog_warning("timer_after: null callback\n");
        return -1;
    }
    if (!inited)
        klog_warning("timer_after before timer_init: delay ignored, task fires on the first tick\n");

    uint64_t need = ms_to_ticks_ceil(ms);
    if (need == UINT64_MAX)
        klog_warning("timer_after: duration saturated, task will effectively never fire\n");

    uint64_t flags = cpu_irq_save();
    int result = -1;

    for (size_t i = 0; i < TIMER_MAX_TASKS; i++) {
        if (tasks[i].id != 0)
            continue;

        int id = timer_alloc_id();
        tasks[i].deadline_ticks = (need > UINT64_MAX - ticks) ? UINT64_MAX : ticks + need;
        tasks[i].cb             = cb;
        tasks[i].arg            = arg;
        tasks[i].id             = id;
        tasks[i].active         = true;
        tasks[i].ready          = false;
        result = id;
        break;
    }

    cpu_irq_restore(flags);

    /* log outside the IRQ-off region */
    if (result < 0)
        klog_warning("timer_after: no free task slot (TIMER_MAX_TASKS=%u)\n",
                     (unsigned)TIMER_MAX_TASKS);
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

/* Pre-IRQ-save reads are safe because: ISR only sets ready false->true;
 * cancel/drain both run in thread context; in_drain blocks reentrancy.
 * Tasks run in slot order, not deadline or scheduling order. */
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

uint32_t timer_hz(void) {
    return (uint32_t)((PIT_FREQ + divisor / 2) / divisor);
}

uint64_t timer_ticks(void) {
    return ticks;
}

uint64_t uptime_ms(void) {
    return ticks * divisor * 1000ull / PIT_FREQ;
}

uint64_t timer_divisor(void) {
    return divisor;
}

size_t timer_tasks_active(void) {
    size_t n = 0;
    for (size_t i = 0; i < TIMER_MAX_TASKS; i++)
        if (tasks[i].id != 0) n++;
    return n;
}

size_t timer_tasks_pending(void) {
    size_t n = 0;
    for (size_t i = 0; i < TIMER_MAX_TASKS; i++)
        if (tasks[i].active && tasks[i].ready) n++;
    return n;
}

/* timer init (hz)
 *
 * 0x34 = 0 0 | 1 1 | 0 1 0 | 0
 *        │ │   │ │   │ │ │   └─ 0: count in binary (1 would be BCD)
 *        │ │   │ │   └─┴─┴──── 010: mode 2
 *        │ │   └─┴──────────── 11: write the divisor as low byte, then high byte
 *        └─┴─────────────────── 00: channel 0
 *
 * Channel	Output	                Usage
 * 0	    IRQ 0 on the PIC	    kernel tick (timer.h->timer_tick())
 * 1	    RAM                     refresh circuitry obsolete, don't use
 * 2	    PC speaker	            beeper and sometimes a calibration source
 *
 * Mode 	Name	                    Behaviour
 * 0        interrupt	                fires once, until re-programmed
 * 1	    hardware one-shot	        waits for a gate signal first
 * 2	    rate generator	            fires, reloads the divisor itself, repeats forever
 * 3	    square wave	                repeats like mode 2, but the output is a 50% duty cycle wave
 * 4	    software strobe	            one pulse after a programmed delay
 * 5	    hardware strobe	            one pulse, started by a gate signal
*/
void timer_init(uint32_t hz) {
    if (inited) {
        klog_warning("timer_init called twice, ignoring...\n");
        return;
    }

    uint32_t want = (hz == 0) ? TIMER_HZ : hz;   /* TIMER_HZ = 1000hz */
    uint64_t d = PIT_FREQ / want;
    bool clamped = false;
    if (d < 1)     { d = 1;     clamped = true; }
    if (d > 65535) { d = 65535; clamped = true; }   /* 16-bit counter */
    divisor = d;

    outb(PIT_CMD, 0x34);            /* channel 0, lobyte/hibyte, mode 2 (rate generator) */
    outb(PIT_CH0, (uint8_t)(d & 0xFF));
    outb(PIT_CH0, (uint8_t)(d >> 8));

    if (irq_register(0, timer_irq, NULL) != 0)
        PANIC("timer: IRQ0 already taken");
    inited = true;

    if (clamped)
        klog_warning("timer_init: %u Hz is out of range, running at %u Hz\n",
                     (unsigned)want, (unsigned)timer_hz());
    klog("• TIMER    [OK] freq=%uHz, pfreq=%uHz, divisor=%u, clamped=%s\n",
                    (unsigned)timer_hz(),
                    (unsigned)PIT_FREQ,
                    (unsigned)timer_divisor(),
                    clamped ? "true" : "false");
}
