#include <stdbool.h>
#include <stdint.h>
#include <core/dev/console.h>
#include <core/kprintf.h>
#include <core/cmd.h>
#include <core/ext.h>

// codebase: 0.2.0

/* Superloop status */
#include <ext/tools/slstat.h>
SYM_COMMAND(sls, "", "Display superloop status", cmd_loopstat_fn);

/* ISA test */
#include <ext/tools/isatest.h>
SYM_COMMAND(isatest, "", "Perform ISA peripheral test", cmd_isatest_fn);

/* Memory test */
#include <ext/tools/memtest.h>
SYM_COMMAND(memtest, "", "Perform Memory test", cmd_memtest_fn);


/* extension greeter */
static int tools_ext_init(void) { return 0; }
SYM_EXTENSION(tools, tools_ext_init, EXT_PRIO_SERVICE);
