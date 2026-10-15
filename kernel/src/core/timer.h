#ifndef CORE_TIMER_H
#define CORE_TIMER_H

#include <stdint.h>
#include "core/version.h"

/* PIT channel 0 as the system tick. Needs idt_init() and pic_init();
 * the tick only starts arriving once interrupts are enabled with cpu_sti(). */
void timer_init(uint32_t hz);

/* The real tick rate (the PIT divisor is an integer, so it is not exactly hz). */
uint32_t timer_hz(void);

uint64_t timer_ticks(void);

/* Milliseconds since timer_init, computed from the actual divisor so it does
 * not drift (1193182 does not divide evenly by 1000). */
uint64_t uptime_ms(void);

/* Sleep at least `ms` milliseconds with the CPU halted between ticks.
 * Interrupts must be able to run: do not call before cpu_sti(). */
void timer_sleep_ms(uint64_t ms);


/* Scheduled one-shot tasks.
 *
 * timer_after(ms, cb, arg) schedules cb(arg) to run at least `ms` milliseconds
 * from now, and returns a positive task id.
 * timer_cancel(id) drops a pending task; returns 0 on success, -1 if the id is
 * unknown or the task already fired.
 *
 * Callbacks run in thread context from timer_drain(), NOT from the tick ISR.
 * The ISR only marks a task ready. This means a callback may call console_write,
 * vfs_write, klog, or anything else that is not safe in interrupt context.
 *
 * timer_sleep_ms() must NOT be called from inside a callback; it would deadlock
 * the drain. Debug builds assert on this.
 *
 * timer_drain() is called by the keyboard loop between interrupts.
 * One drain site for callbacks run in scheduling order among tasks whose deadline has passed. */

int  timer_after(uint64_t ms, void (*cb)(void *), void *arg);
int  timer_cancel(int id);
void timer_drain(void);

/* Bus interface. Registered by timer_init as "timer" / BUS_CLASS_TIMER. */
struct timer_ops {
    uint64_t (*ticks)(void);
    uint64_t (*uptime_ms)(void);
    uint32_t (*hz)(void);
    void     (*sleep_ms)(uint64_t ms);
    int      (*after)(uint64_t ms, void (*cb)(void *), void *arg);
    int      (*cancel)(int id);
};


/* Convert milliseconds to timer ticks.
 *
 * ms_to_ticks_floor()   the largest tick count that is <= ms milliseconds.
 * ms_to_ticks_ceil()    the smallest tick count that is >= ms milliseconds.
 *                       0 ms maps to 0 ticks (no delay).
 * ms_to_ticks_round()   the nearest tick count (floor and ceil tied).
 *
 * All three treat hz == 0 as "timer not initialised" and return 0.
 * All three saturate on overflow: huge ms values clamp to UINT64_MAX.
 *
 * These are the single source of truth for ms <-> tick conversion in the kernel.
 * Callers that need "at least this long" (timeouts, TTLs, deadlines)
 * use _ceil. Callers that need "at most this long" (rate limiters, throttle intervals) use _floor.
 * Callers that want the closest approximation for display or reporting use _round. */
uint64_t ms_to_ticks_floor(uint64_t ms);
uint64_t ms_to_ticks_ceil (uint64_t ms);
uint64_t ms_to_ticks_round(uint64_t ms);

#endif /* CORE_TIMER_H */
