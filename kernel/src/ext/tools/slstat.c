#include <stdbool.h>
#include <stdint.h>

#include <core/version.h>
#include <core/arch/pmm.h>
#include <core/arch/vmm.h>
#include <core/dev/timer.h>
#include <core/kprintf.h>
#include <core/cmd.h>
#include <core/ext.h>
#include <core/superloop.h>
#include <ext/tools/slstat.h>

int cmd_loopstat_fn(const struct cmd_args *a) {
    (void)a;

    struct superloop_stats sl = superloop_get_stats();

    kprintf("Superloop\n");
    kprintf("  Iterations   : %llu\n", (unsigned long long)sl.iterations);
    kprintf("  Idle         : %llu (%llu%%)\n",
            (unsigned long long)sl.idle_iterations,
            (unsigned long long)(sl.iterations ?
                (sl.idle_iterations * 100) / sl.iterations : 0));
    kprintf("  Halts        : %llu\n", (unsigned long long)sl.halts);

    kprintf("Clock\n");
    kprintf("  HZ           : %u (PIT=%llu)\n", timer_hz(), PIT_FREQ);
    kprintf("  Ticks        : %llu\n",
                            (unsigned long long)timer_ticks());
    kprintf("  Uptime       : %llu ms\n",
                            (unsigned long long)uptime_ms());
    kprintf("  Tasks        : %llu active, %llu pending\n",
                            (unsigned long long)timer_tasks_active(),
                            (unsigned long long)timer_tasks_pending());

    kprintf("Roster (%u active)\n", sl.participants);
    kprintf("  name        calls        ticks        max  budget  status\n");
    kprintf("  ----------  -----------  -----------  ---  ------  ------\n");

    for (uint32_t i = 0; i < sl.participants; i++) {
        struct superloop_participant_stats p;
        if (!superloop_get_participant(i, &p)) break;
        kprintf("  %-10s  %-11llu  %-11llu  %-3llu  %-6u  %s %s\n",
                p.name,
                (unsigned long long)p.calls,
                (unsigned long long)p.total_ticks,
                (unsigned long long)p.max_ticks,
                p.warm_up_remaining,
                p.warned ? "slow" : "healthy",
                p.warned & p.struggler ? "(jitter)" : "");
    }
    return CMD_OK;
}
