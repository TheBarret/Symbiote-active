#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <lib/string.h>
#include <core/version.h>
#include <core/arch/cpu.h>
#include <core/dev/keyboard.h>
#include <core/dev/console.h>
#include <core/cmd.h>
#include <core/kprintf.h>
#include <core/shell.h>


// Moved to ref: core/version.h
//#define SHELL_LINE_MAX  128

/* argv[0] is the command name; the rest are its arguments.
 * So thetokenizer's cap is one more than the maximum argument count a command may declare. */
#define SHELL_ARG_MAX   (CMD_MAX_ARGS + 1)

/*  line input  */

/* Read one line from the keyboard into buf (up to cap-1 chars).
 * Handles backspace and echoes as it goes.
 *
 * Returns the length on Enter, or (size_t)-1 if the line was aborted (Ctrl-C). An empty line returns 0. */
static size_t read_line(char *buf, size_t cap) {
    size_t len = 0;
    for (;;) {
        int c = kbd_getchar();

        if (c == '\n' || c == '\r') {
            console_putc('\n');
            break;
        }

        if (c == 0x03) {            /* Ctrl-C: abandon the line */
            console_putc('\n');
            return (size_t)-1;
        }

        if (c == '\b' || c == 0x7F) { /* Backspace / DEL */
            if (len > 0) {
                len--;
                console_putc('\b');
                console_putc(' ');
                console_putc('\b');
            }
            continue;
        }

        if (c < 0x20 || c > 0x7E)
            continue;

        if (len + 1 >= cap)
            continue;

        buf[len++] = (char)c;
        console_putc((char)c);
    }
    buf[len] = '\0';
    return len;
}

/*  tokenizer  */

/* Split line in place into argv. Returns argc, or -1 if the line has more than `max` words.
 *
 * On success, argv[argc] is set to NULL so callers that want a NULL-terminated vector (rather than an (argc, argv) pair) can use it.
 * The caller must provide argv with room for max + 1 entries. */
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
    /* One extra slot for the NULL terminator that tokenize installs. */
    char *argv[SHELL_ARG_MAX + 1];
    int argc = tokenize(line, argv, SHELL_ARG_MAX);
    if (argc < 0) {
        kprintf("Too many words (max=%d including the command name)\n",
                SHELL_ARG_MAX);
        return;
    }
    if (argc == 0)
        return;

    /* Everything, including help, goes through the registry. If nothing is registered under that name, cmd_dispatch reports it. */
    cmd_dispatch(argc, argv);
}

/*  the loop  */

void shell_run(void) {
    if (!kbd_init()) {
        kprintf("Error: no PS/2 keyboard detected!\n");
        cpu_halt_forever();
    }

    int problems = cmd_selfcheck();
    if (problems) {
        kprintf("[Warning] %d issues detected in look-up table.\n", problems);
    }
    kprintf("\nType 'help' for a list of commands, ready when you are!\n");

    char line[SHELL_LINE_MAX];
    for (;;) {
        kprintf("> ");
        size_t len = read_line(line, sizeof line);
        if (len == (size_t)-1)
            continue;               /* aborted with Ctrl-C */
        if (len == 0)
            continue;
        run_line(line);
    }
}
