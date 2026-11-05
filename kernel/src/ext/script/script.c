#include <stdbool.h>
#include <core/cmd.h>
#include <core/kprintf.h>
#include <core/ext.h>
#include <ext/script/decimal.h>

static int cmd_script_fn(const struct cmd_args *a) {
    (void)a;
    test_decimal();
    kprintf("\n");
    return CMD_OK;
}

SYM_COMMAND(dectest, "", "Decimal Test", cmd_script_fn);

/* extension greeter */
static int dec_ext_init(void) { return 0; }
SYM_EXTENSION(dec_test, dec_ext_init, EXT_PRIO_APPLET);
