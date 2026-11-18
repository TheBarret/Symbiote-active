#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <core/dev/timer.h>
#include <core/klog.h>
#include <core/superloop.h>

/* codebase: 0.2.0 */

static struct superloop_participant *g_parts[SL_MAX_PARTICIPANTS];
static uint32_t g_part_count;

/* metrics variables */
static uint64_t g_iterations;
static uint64_t g_idle_iterations;
static uint64_t g_halts;

/* Insert
 * Sort by priority:
 * - table stays ordered so the loop is a plain sequential walk.
 * - n <= SL_MAX_PARTICIPANTS, so this is trivially cheap.
 */
static void sort_by_priority(void) {
    for (uint32_t i = 1; i < g_part_count; i++) {
        struct superloop_participant *p = g_parts[i];
        uint32_t j = i;
        while (j > 0 && g_parts[j - 1]->priority > p->priority) {
            g_parts[j] = g_parts[j - 1];
            j--;
        }
        g_parts[j] = p;
    }
}

bool superloop_add(struct superloop_participant *p) {
    if (p == NULL || p->tick == NULL || p->name == NULL) return false;
    if (g_part_count >= SL_MAX_PARTICIPANTS) return false;
    for (uint32_t i = 0; i < g_part_count; i++)
        if (g_parts[i]->name == p->name)
            return false;   /* name collision (pointer equality) */
    g_parts[g_part_count++] = p;
    sort_by_priority();
    return true;
}

/* run one-tick */
void superloop_prime(void) {
    for (uint32_t i = 0; i < g_part_count; i++) {
        (void)g_parts[i]->tick();
    }
}

__attribute__((noreturn))
void superloop_run(void (*halt_fn)(void)) {
    for (;;) {
        bool any_work = false;

        for (uint32_t i = 0; i < g_part_count; i++) {
            struct superloop_participant *p = g_parts[i];

            uint64_t t0   = timer_ticks();
            bool     did  = p->tick();
            uint64_t t1   = timer_ticks();
            uint64_t dt   = t1 - t0;

            p->calls++;
            p->total_ticks += dt;
            if (dt > p->max_ticks) p->max_ticks = dt;

            /* strugglers are not pressured */
            if (!p->struggler) {
                if (dt >= SL_WARN_TICKS) {
                    if (p->warm_up_remaining > 0) {
                        p->warm_up_remaining--;
                    } else if (!p->warned) {
                        p->warned = true;
#ifdef SYM_MEMDEBUG
                        klog_warning("Watchdog: high latency at task=%s, observed=%llu ticks, trigger=%u ticks\n",
                                        p->name, (unsigned long long)dt, (unsigned)SL_WARN_TICKS);
#endif
                    }
                }
            }
            if (did) any_work = true;
        }

        g_iterations++;
        if (!any_work) g_idle_iterations++;

        g_halts++;
        halt_fn();
    }
}

struct superloop_stats superloop_get_stats(void) {
    struct superloop_stats s = {
        .iterations      = g_iterations,
        .idle_iterations = g_idle_iterations,
        .halts           = g_halts,
        .participants    = g_part_count,
    };
    return s;
}

bool superloop_get_participant(uint32_t index, struct superloop_participant_stats *out) {
    if (out == NULL || index >= g_part_count) return false;
    struct superloop_participant *p = g_parts[index];
    out->name              = p->name;
    out->calls             = p->calls;
    out->total_ticks       = p->total_ticks;
    out->max_ticks         = p->max_ticks;
    out->warm_up_remaining = p->warm_up_remaining;
    out->warned            = p->warned;
    out->struggler         = p->struggler;
    return true;
}
