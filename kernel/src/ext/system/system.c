#include <stddef.h>
#include <stdint.h>
#include <lib/string.h>
#include <core/version.h>
#include <core/arch/cpu.h>
#include <core/arch/isr.h>
#include <core/dev/keyboard.h>
#include <core/dev/console.h>
#include <core/dev/timer.h>
#include <core/kprintf.h>
#include <core/panic.h>
#include <core/cmd.h>
#include <core/ext.h>

/* System-level commands & utilities */

/*  irqs  */
#define U(x) ((unsigned long long)(x))


/*  version  */

static int cmd_version_fn(const struct cmd_args *a) {
    (void)a;
    kprintf("Symbiote %s\n", SYM_VERSION);
    return CMD_OK;
}

SYM_COMMAND(version, "", "Display kernel version", cmd_version_fn);

/*  clear  */

static int cmd_clear_fn(const struct cmd_args *a) {
    (void)a;
    /* ANSI: ESC[2J clears the screen, ESC[H homes the cursor. */
    kprintf("\x1b[2J\x1b[H");
    return CMD_OK;
}

SYM_COMMAND(clear, "", "Clear screen (ANSI)", cmd_clear_fn);

/*  halt  */
static int cmd_halt_fn(const struct cmd_args *a) {
    (void)a;
    kprintf("Bye!\n");
    cpu_halt_forever();
    return CMD_OK;      /* not reached */
}
SYM_COMMAND(halt, "", "Stop Machine", cmd_halt_fn);


/*  irqs  */

static int cmd_irqs(const struct cmd_args *a) {
    (void)a;
    kprintf("irq  handler   count        spurious  unhandled\n");
    for (int i = 0; i < 16; i++) {
        struct irq_info in = irq_get_info(i);
        if (!in.registered && in.count == 0 && in.spurious == 0 && in.unhandled == 0)
            continue;
        kprintf("%3d  %-8s  %-11llu  %-8llu  %llu\n", i, in.registered ? "yes" : "-",
                U(in.count), U(in.spurious), U(in.unhandled));
    }
    return 0;
}
SYM_COMMAND(irqs, "", "IRQ interrupt status", cmd_irqs);

/*  uptime */

static int cmd_uptime(const struct cmd_args *a) {
    (void)a;
    uint64_t ms = uptime_ms();
    kprintf("uptime %llu.%03llu seconds, %llu ms, %llu ticks at %u Hz\n",
            U(ms / 1000), U(ms % 1000), U(ms), U(timer_ticks()), timer_hz());
    return 0;
}
SYM_COMMAND(uptime, "", "Time since boot", cmd_uptime);

/*  sleep */
/*
static int cmd_sleep(const struct cmd_args *a) {
    uint64_t remaining = a->num[0];
    uint64_t start = uptime_ms();

    //Sleep in short slices so Esc can interrupt a long one.
    while (remaining) {
        uint64_t slice = remaining > 20 ? 20 : remaining;
        timer_sleep_ms(slice);
        remaining -= slice;

        char c = 0;
        if (kbd_poll(&c) == KBD_SPECIAL && c == KBD_KEY_ESC) {
            kprintf("sleep: interrupted after %llu ms\n", U(uptime_ms() - start));
            return 1;
        }
    }
    kprintf("slept %llu ms\n", U(uptime_ms() - start));
    return 0;
}
SYM_COMMAND(sleep, "<ms:num>", "Sleep N milliseconds (Esc=interrupt)", cmd_sleep);
 */

static int cmd_help(const struct cmd_args *a) {
    /* No argument: list every registered command with its one-line help. */
    if (a->argc == 0) {
        size_t n = cmd_count();
        kprintf("%zu command%s:\n", n, n == 1 ? "" : "s");
        for (size_t i = 0; i < n; i++) {
            const struct sym_cmd *c = cmd_at(i);
            kprintf("  %-14s %s\n", c->name, c->help);
        }
        return CMD_OK;
    }

    /* One or more names: print the info block for each. */
    int rc = CMD_OK;
    for (int i = 0; i < a->argc; i++) {
        const struct sym_cmd *c = cmd_find(a->argv[i]);
        if (!c) {
            kprintf("%s: no such command\n", a->argv[i]);
            rc = 1;
            continue;
        }
        kprintf("%s — %s\n", c->name, c->help);
        cmd_print_usage(c);
    }
    return rc;
}

SYM_COMMAND(help, "[command:str...]", "List all commands or inspect with help <cmd>", cmd_help);

/* extension greeter */
static int system_ext_init(void) { return 0; }
SYM_EXTENSION(sys_toolkit, system_ext_init, EXT_PRIO_APPLET);
