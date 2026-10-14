#include "klog.h"
#include <limits.h>
#include <stddef.h>
#include <core/version.h>
#include <core/ext.h>
#include <core/klog.h>

/* Provided by the linker script (linker-scripts/x86_64.lds). */
extern const struct sym_ext __symbiote_ext_start[];
extern const struct sym_ext __symbiote_ext_end[];

static const char *current;

const char *ext_current(void) { return current; }

size_t ext_count(void) {
    return (size_t)(__symbiote_ext_end - __symbiote_ext_start);
}

void ext_init_all(void) {
    /* Walk priority levels in ascending order:
     * each pass finds the next level above `last`, then runs everything at that level.
     * The table is read-only, so we do it with repeated scans instead of sorting. */
    int last = -1;
    for (;;) {
        int next = INT_MAX;
        for (const struct sym_ext *e = __symbiote_ext_start; e < __symbiote_ext_end; e++)
            if (e->prio > last && e->prio < next)
                next = e->prio;
        if (next == INT_MAX)
            break;

        for (const struct sym_ext *e = __symbiote_ext_start; e < __symbiote_ext_end; e++) {
            if (e->prio != next)
                continue;
            current = e->name;
            int rc = e->init();
            current = NULL;
            if (rc == 0)
                klog_ext("→ %-40s enabled\n", e->name);
            else
                klog_error("→ %-40s failed, %d\n", e->name, rc);
        }
        last = next;
    }
}
