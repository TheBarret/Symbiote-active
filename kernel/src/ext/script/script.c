#include <stdbool.h>
#include <core/cmd.h>
#include <core/kprintf.h>
#include <core/ext.h>
#include <ext/script/vm16.h>

/* extension greeter */
static int vm16_ext_init(void) { return vm_ext_init(); }

SYM_EXTENSION(vm16, vm16_ext_init, EXT_PRIO_APPLET);
