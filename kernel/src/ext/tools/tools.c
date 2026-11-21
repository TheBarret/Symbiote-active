#include <stdbool.h>
#include <stdint.h>
#include <core/dev/console.h>
#include <core/kprintf.h>
#include <core/cmd.h>
#include <core/ext.h>
#include <core/services/router.h>

// codebase: 0.2.0

/* Memory test */
#include <ext/tools/memtest.h>
SYM_COMMAND(memtest, "", "Perform Memory test", cmd_memtest_fn);

/*  router stats  */
static int cmd_router_stats_fn(const struct cmd_args *a) {
    (void)a;
    router_stats();
    return CMD_OK;
}
SYM_COMMAND(rst, "", "Display router status", cmd_router_stats_fn);

/* extension greeter */
static int tools_ext_init(void) { return 0; }
SYM_EXTENSION(extra, tools_ext_init, EXT_PRIO_APPLET);
