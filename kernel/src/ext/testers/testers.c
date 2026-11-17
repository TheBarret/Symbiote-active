#include <stdbool.h>
#include <stdint.h>
#include <core/dev/console.h>
#include <core/kprintf.h>
#include <core/cmd.h>
#include <core/ext.h>

// codebase: 0.2.0

/* ISA test */
#include <ext/testers/isatest.h>
SYM_COMMAND(isatest, "", "Perform ISA peripheral test", cmd_isatest_fn);

/* Memory test */
#include <ext/testers/memtest.h>
SYM_COMMAND(memtest, "", "Perform Memory test", cmd_memtest_fn);


/* extension greeter */
static int testers_ext_init(void) { return 0; }
SYM_EXTENSION(testing_toolkit, testers_ext_init, EXT_PRIO_APPLET);
