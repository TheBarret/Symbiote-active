#ifndef CORE_SUPERLOOP_H
#define CORE_SUPERLOOP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* codebase: 0.2.0 */

/* Superloop:
 * A primitive single thread watchdog using timer.h
 * approx: millisecond resolution from 1000 Hz
 *
 * Participant contract:
 *   - `tick` runs in thread context, must be bounded, must not block.
 *   - returns true if the participant did any work this iteration,
 *     false if idle (used for idle_iterations accounting).
 *   - must not re-enter the superloop, must not call superloop_add.
 *
 * The loop is NOT a scheduler: no preemption, no yield, no threading.
 */

typedef bool (*superloop_tick_fn)(void);

struct superloop_participant {
    const char       *name;
    superloop_tick_fn tick;
    uint8_t           priority;    /* lower = earlier in the loop */

    /* Warm-up budget: number of over-threshold iterations absorbed silently
     * before the watchdog arms for this participant.
     * Decrements on each over-threshold call (not per iteration):
     * a participant that idles for a million iterations still has its full budget.
     */
    uint32_t          warm_up_remaining;

    /* struggler mode (fixed waiters, kbd, isa) */
    bool              struggler;

    /* Runtime state (owned by the superloop; do not touch). */
    uint64_t calls;
    uint64_t total_ticks;          /* sum of per-call tick deltas */
    uint64_t max_ticks;            /* largest single-call tick delta */
    bool     warned;               /* watchdog warning already logged */
};

/* Register a participant. R
 * eturns true on success, false on NULL, name collision, or table full.
 * The struct itself must have static lifetime (the loop stores the pointer, not a copy).
 */
bool superloop_add(struct superloop_participant *p);

/* Call each registered participant's tick once, discarding timing,  watchdog and counters.
 * Warms cold paths so the first timed iteration in superloop_run is representative rather than a cold-start outlier.
 */
void superloop_prime(void);

/* Run the loop. `halt_fn` is called at the end of each iteration;
 * typically cpu_sti_hlt, but a no-op for tests. Does not return under normal use. */
__attribute__((noreturn))
void superloop_run(void (*halt_fn)(void));

/* Stats snapshot. */
struct superloop_stats {
    uint64_t iterations;
    uint64_t idle_iterations;      /* iterations where every participant returned false */
    uint64_t halts;                /* times halt_fn was called */
    uint32_t participants;         /* registered participant count */
};

/* Per-participant snapshot. Index 0..participants-1 in priority order. */
struct superloop_participant_stats {
    const char *name;
    uint64_t    calls;
    uint64_t    total_ticks;
    uint64_t    max_ticks;
    uint32_t    warm_up_remaining;
    bool        warned;
    bool        struggler;
};

struct superloop_stats superloop_get_stats(void);
bool superloop_get_participant(uint32_t index, struct superloop_participant_stats *out);


/* Aggregate loop-level + per-participant state into one callable snapshot.
 * This is the superloop's own view; callers that also want pmm/vmm/timer
 * stats compose those themselves (the superloop does NOT know about them).
struct superloop_snapshot {
    struct superloop_stats stats;
    uint32_t participants;  // for the caller to size its participant array
};
struct superloop_snapshot superloop_snapshot(void);
 */

#endif /* CORE_SUPERLOOP_H */
