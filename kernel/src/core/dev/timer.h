#ifndef CORE_TIMER_H
#define CORE_TIMER_H

#include <stddef.h>
#include <stdint.h>
#include <core/version.h>

/* codebase: 0.2.0 */

/* changelog:
 * Task ids: next_task_id++ relied on signed overflow, which is undefined behaviour, it now wraps from INT32_MAX to 1 explicitly.
 * timer_sleep_ms now warns when called before init, It also treats a saturated duration as unrepresentable.
 * Before, a saturated value could produce a sleep that never ends if called when ticks was still 0.
 * timer_after: it now warns when: * the callback is NULL no slot is free (the log call happens after interrupts are restored)
 * it's called before init, which was already designed to fire on the first tick the duration saturated
 * timer_init: the log used to print the requested frequency, which is wrong when the divisor is clamped or rounded.
 * It now prints the real timer_hz(), and warns when the request was out of range.
 * ready flag: it's now volatile, so the compiler can't cache it between the IRQ and drain.
 * On x86 it already worked because of the surrounding barriers, but now the code says so.
 */

/* PIT channel 0 as the system tick. Needs idt_init() and pic_init();
 * the tick only starts arriving once interrupts are enabled with cpu_sti().
 * Double-call is warn-no-op. A request outside what the 16-bit divisor can
 * produce is clamped, with a warning, and timer_hz() reports the real rate. */
void timer_init(uint32_t hz);

/* The real tick rate (the PIT divisor is an integer, so it is not exactly hz). */
uint32_t timer_hz(void);

uint64_t timer_ticks(void);

/* Milliseconds since timer_init, computed from the actual divisor so it does
 * not drift (1193182 does not divide evenly by 1000). */
uint64_t uptime_ms(void);

/* Sleep at least `ms` milliseconds with the CPU halted between ticks.
 * Interrupts must be able to run: do not call before cpu_sti().
 * Before timer_init, or for a duration that cannot be represented, it logs a
 * warning and returns without sleeping. */
void timer_sleep_ms(uint64_t ms);


/* Scheduled one-shot tasks.
 *
 * timer_after(ms, cb, arg) schedules cb(arg) to run at least `ms` milliseconds
 * from now, and returns a positive task id. It returns -1, with a warning, if
 * cb is NULL or all TIMER_MAX_TASKS slots are in use. Before timer_init the
 * delay is ignored (with a warning) and the task fires on the first tick. A
 * duration too large to represent saturates (with a warning) and effectively
 * never fires.
 * timer_cancel(id) drops a pending task; returns 0 on success, -1 if the id is
 * unknown or the task already fired.
 *
 * Callbacks run in thread context from timer_drain(), NOT from the tick ISR.
 * The ISR only marks a task ready. This means a callback may call console_write,
 * vfs_write, klog, or anything else that is not safe in interrupt context.
 *
 * timer_sleep_ms() must NOT be called from inside a callback; it would stall
 * the drain for the sleep's duration. All builds PANIC on this.
 *
 * timer_drain() is called from the DRAIN loop task (run_drain in main.c).
 * Tasks whose deadline has passed run in slot order, which is neither
 * deadline order nor scheduling order. */

/* timer_irq is O(TIMER_MAX_TASKS) per tick; keep the constant small or move to a heap if it grows. */
int  timer_after(uint64_t ms, void (*cb)(void *), void *arg);
int  timer_cancel(int id);
void timer_drain(void);

/* Convert milliseconds to timer ticks.
 *
 * ms_to_ticks_floor()   the largest tick count that is <= ms milliseconds.
 * ms_to_ticks_ceil()    the smallest tick count that is >= ms milliseconds.
 *                       0 ms maps to 0 ticks (no delay).
 * ms_to_ticks_round()   the nearest tick count (ties round up).
 *
 * All three return 0 if the timer is not initialised (timer_init not yet called).
 * All three saturate on overflow: huge ms values clamp to UINT64_MAX, which
 * callers must treat as "unrepresentable".
 *
 * These are the single source of truth for ms <-> tick conversion in the kernel.
 * Callers that need "at least this long" (timeouts, TTLs, deadlines)
 * use _ceil. Callers that need "at most this long" (rate limiters, throttle intervals) use _floor.
 * Callers that want the closest approximation for display or reporting use _round. */
uint64_t ms_to_ticks_floor(uint64_t ms);
uint64_t ms_to_ticks_ceil (uint64_t ms);
uint64_t ms_to_ticks_round(uint64_t ms);


/* Timer health introspection (ref: metrics.c). */
uint64_t timer_divisor(void);       /* the PIT divisor actually programmed */
size_t   timer_tasks_active(void);  /* slots with id != 0 */
size_t   timer_tasks_pending(void); /* slots with active && ready, awaiting drain */

#endif /* CORE_TIMER_H */
