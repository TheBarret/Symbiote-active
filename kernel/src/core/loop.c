#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <core/arch/cpu.h>
#include <core/dev/timer.h>
#include <core/loop.h>

static struct task *g_tasks[LOOP_MAX_TASKS];
static uint32_t     g_count;
static volatile uint32_t g_ready;     /* written by ISRs: atomics only */
static uint64_t g_passes, g_halts;

void loop_wake(struct task *t) {
    if (t == NULL || t->bit == 0) return;
    __atomic_fetch_or(&g_ready, t->bit, __ATOMIC_RELEASE);
}

bool loop_add(struct task *t) {
    if (t == NULL || t->run == NULL || t->name == NULL || t->budget == 0) return false;
    if (g_count >= LOOP_MAX_TASKS) return false;
    t->bit      = 1u << g_count;
    t->next_due = timer_ticks() + t->period;
    g_tasks[g_count++] = t;
    loop_wake(t);                     /* first run doubles as warm-up */
    return true;
}

static void arm_periodic(uint64_t now) {
    for (uint32_t i = 0; i < g_count; i++) {
        struct task *t = g_tasks[i];
        if (t->period != 0 && now >= t->next_due) {
            t->next_due = now + t->period;
            loop_wake(t);
        }
    }
}

static void run_task(struct task *t) {
    /* clear BEFORE running so a wake during run() is not lost */
    __atomic_fetch_and(&g_ready, ~t->bit, __ATOMIC_ACQ_REL);

    uint64_t t0   = timer_ticks();
    uint32_t used = t->run(t->ctx, t->budget);
    uint64_t dt   = timer_ticks() - t0;

    t->runs++;
    t->units += used;
    if (used > t->max_units) t->max_units = used;
    if (dt > t->max_ticks)   t->max_ticks = dt;
    if (t->limit_ticks != 0 && dt > t->limit_ticks) t->slow++;

    if (used >= t->budget) loop_wake(t);   /* cut off: still has work */
}

__attribute__((noreturn))
void loop_run(void (*idle_fn)(void)) {
    for (;;) {
        arm_periodic(timer_ticks());

        uint32_t pass = __atomic_load_n(&g_ready, __ATOMIC_ACQUIRE);
        while (pass != 0) {
            uint32_t i = (uint32_t)__builtin_ctz(pass);
            pass &= pass - 1;
            run_task(g_tasks[i]);
        }
        g_passes++;

        /* Idle decision: check and halt with no window for a lost wakeup. */
        cpu_cli();
        if (__atomic_load_n(&g_ready, __ATOMIC_ACQUIRE) == 0) {
            g_halts++;
            if (idle_fn != NULL) idle_fn();   /* sti; hlt */
            else cpu_sti();
        } else {
            cpu_sti();
        }
    }
}

struct loop_stats loop_get_stats(void) {
    struct loop_stats s = { .passes = g_passes, .halts = g_halts, .tasks = g_count };
    return s;
}

const struct task *loop_get_task(uint32_t index) {
    return (index < g_count) ? g_tasks[index] : NULL;
}
