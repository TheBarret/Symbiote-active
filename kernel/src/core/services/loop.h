#ifndef CORE_SERVICES_LOOP_H
#define CORE_SERVICES_LOOP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Run Loop
 * - Event-driven partitioning of the single thread
 * - Not a scheduler: no preemption, no yield, no threads
 *
 * A task runs only when its ready bit is set.
 * Bits are set by loop_wake() (ISR-safe), by the task's own period,
 * or by the loop itself when a task consumed its whole budget.
 * The CPU halts only when no bit is set.
 *
 * run(ctx, budget) contract:
 *   - do at most `budget` units of work, in thread context, never block;
 *   - return units consumed; < budget means drained (task goes to sleep),
 *     >= budget means cut off (task stays ready).
 *
 * Registration order is priority order (first added runs first).
 *
 */

#define LOOP_MAX_TASKS 32u

typedef uint32_t (*task_run_fn)(void *ctx, uint32_t budget);

struct task {
    /* configuration (set before loop_add) */
    const char  *name;
    task_run_fn  run;
    void        *ctx;
    uint32_t     budget;        /* max units per slice, must be > 0 */
    uint32_t     period;        /* wake every N ticks; 0 = event-driven only */
    uint32_t     limit_ticks;   /* expected max wall ticks per slice; 0 = unchecked */

    /* runtime (owned by the loop) */
    uint32_t     bit;
    uint64_t     next_due;
    uint64_t     runs;
    uint64_t     units;
    uint32_t     max_units;
    uint64_t     max_ticks;
    uint64_t     slow;          /* slices that exceeded limit_ticks */
};

bool loop_add(struct task *t);          /* static lifetime; first run is immediate */
void loop_wake(struct task *t);         /* ISR-safe, idempotent */

/* idle_fn is called with interrupts DISABLED and no work pending;
 * it must re-enable them (cpu_sti_hlt). NULL = just re-enable (tests). */
__attribute__((noreturn))
void loop_run(void (*idle_fn)(void));

struct loop_stats {
    uint64_t passes;
    uint64_t halts;
    uint32_t tasks;
};
struct loop_stats loop_get_stats(void);
const struct task *loop_get_task(uint32_t index);

#endif /* CORE_LOOP_H */
