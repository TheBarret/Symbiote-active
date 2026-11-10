#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <lib/string.h>
#include <core/version.h>
#include <core/arch/cpu.h>
#include <core/dev/keyboard.h>
#include <core/dev/console.h>
#include <core/dev/timer.h>
#include <core/cmd.h>
#include <core/kprintf.h>
#include <core/shell.h>

#define SHELL_ARG_MAX   (CMD_MAX_ARGS + 1)

/*  state  */

static char   line_buf[SHELL_LINE_MAX];
static size_t line_len;
static bool   prompt_shown;

/*  tokenizer  */

/* Split line in place into argv. Returns argc, or -1 if the line has more than `max` words.
 * On success, argv[argc] is set to NULL. */
static int tokenize(char *line, char **argv, int max) {
    int argc = 0;
    char *p = line;
    for (;;) {
        while (*p == ' ' || *p == '\t')
            *p++ = '\0';
        if (!*p)
            break;
        if (argc == max)
            return -1;
        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t')
            p++;
    }
    argv[argc] = NULL;
    return argc;
}

/*  one line  */

static void run_line(char *line) {
    char *argv[SHELL_ARG_MAX + 1];
    int argc = tokenize(line, argv, SHELL_ARG_MAX);
    if (argc < 0) {
        kprintf("Too many words (max=%d including the command name)\n", SHELL_ARG_MAX);
        return;
    }
    if (argc == 0)
        return;
    cmd_dispatch(argc, argv);
}

/*  public API  */

void shell_init(void) {
    line_len = 0;
    prompt_shown = false;
    line_buf[0] = '\0';
}

void shell_tick(char c) {
    if (!prompt_shown) {
        kprintf("> ");
        prompt_shown = true;
    }

    if (c == 0) return; /* No character available, just yielding */

    if (c == '\n' || c == '\r') {
        console_putc('\n');
        prompt_shown = false;
        line_buf[line_len] = '\0';
        run_line(line_buf);
        line_len = 0;
        return;
    }

    if (c == 0x03) { /* Ctrl-C */
        console_putc('\n');
        prompt_shown = false;
        line_len = 0;
        return;
    }

    if (c == '\b' || c == 0x7F) {
        if (line_len > 0) {
            line_len--;
            console_putc('\b');
            console_putc(' ');
            console_putc('\b');
        }
        return;
    }

    if (c < 0x20 || c > 0x7E)
        return;

    if (line_len + 1 >= SHELL_LINE_MAX)
        return;

    line_buf[line_len++] = c;
    console_putc(c);
}

/*  legacy blocking loop  */

void shell_run(void) {
    int problems = cmd_selfcheck();
    if (problems) {
        kprintf("[Warning] %d issues detected in look-up table.\n", problems);
    }
    kprintf("\nType 'help' for a list of commands, ready when you are!\n");

    shell_init();

    for (;;) {
        char c;
        enum kbd_event ev = kbd_wait(&c);

        /* In the blocking model, we still must drain timers so that
         * scheduled tasks don't starve while waiting for a keystroke. */
        timer_drain();

        if (ev == KBD_CHAR) {
            shell_tick(c);
        }

        cpu_sti_hlt();
    }
}
