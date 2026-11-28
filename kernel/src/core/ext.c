#include <limits.h>
#include <stddef.h>
#include <core/version.h>
#include <core/ext.h>
#include <core/klog.h>
#include <core/services/logger.h>

/* codebase: 0.2.0 */

/* Provided by the linker script (linker-scripts/x86_64.lds). */
extern const struct sym_ext __symbiote_ext_start[];
extern const struct sym_ext __symbiote_ext_end[];

static const char *current;

const char *ext_current(void) { return current; }

size_t ext_count(void) {
    return (size_t)(__symbiote_ext_end - __symbiote_ext_start);
}

/* Walk priority levels in ascending order: each pass finds the next level
 * above `last`, then runs everything at that level. Within a level the order
 * is the order the linker placed the descriptors (the EXTENSIONS order).
 * The table is read-only, so repeated scans replace a sort. `long long`
 * keeps every int prio reachable, negative ones included.
 */
void ext_init_all(void) {
    log_write("ext_init_all() start\n");
    long long last = (long long)INT_MIN - 1;
    for (;;) {
        long long next = LLONG_MAX;
        for (const struct sym_ext *e = __symbiote_ext_start; e < __symbiote_ext_end; e++)
            if (e->prio > last && e->prio < next)
                next = e->prio;
        if (next == LLONG_MAX)
            break;

        for (const struct sym_ext *e = __symbiote_ext_start; e < __symbiote_ext_end; e++) {
            if (e->prio != next)
                continue;
            if (!e->name || !e->init) {
                klog_error("→ extension at prio %d is incomplete, skipped\n", e->prio);
                continue;
            }
            const char *prev = current;
            current = e->name;
            int rc = e->init();
            current = prev;
            if (rc == 0)
                klog_ext("[%d] → %-50s enabled\n", e->prio, e->name);
            else
                klog_error("→ %-40s failed, %d\n", e->name, rc);
        }
        last = next;
    }
    log_write("ext_init_all() stop\n");
}
