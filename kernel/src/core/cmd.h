#ifndef CORE_CMD_H
#define CORE_CMD_H

#include <stddef.h>
#include <stdint.h>

/* PLUG SLOT: shell command.
 *
 * A command is one descriptor in the ".symbiote_cmd" linker section,
 * exactly like an extension is one descriptor in ".symbiote_ext".
 * The shell never has a list of commands: it tokenizes a line and asks this registry to run it.
 * To add a command, write a function and one SYM_COMMAND() line,
 * in any file that is built into the image. To remove it, drop the file from EXTENSIONS.
 *
 * Each command also DECLARES the parameters it wants.
 * The dispatcher checks the call against that declaration before the handler ever runs,
 * so a handler only sees arguments that are already valid.
 *
 * Parameter spec (a string), one item per parameter, separated by spaces:
 *
 *     <name:type>      required
 *     [name:type]      optional (all optional ones come after required ones)
 *     <name:type...>   the last parameter may be repeated ("rest"):
 *     [name:type...]   one or more / zero or more
 *
 * Types:  num   unsigned 64-bit integer, decimal or 0x hex (converted)
 *         str   any text
 *
 * Examples:   ""                      takes nothing
 *             "<addr:num> [len:num]"  an address, optionally a length
 *             "[text:str...]"         any number of words
 *
 * The spec doubles as the usage text, so it never goes out of date:
 *     peek: missing <addr:num>
 *     usage: peek <addr:num> [len:num]
 */

#define CMD_MAX_ARGS 8      /* most arguments one command can take */

/* What a handler receives. Everything in here has already been validated. */
struct cmd_args {
    int                argc;                /* arguments given (command name NOT counted) */
    const char *const *argv;                /* argv[0] is the first argument. Points into the shell's
                                             * line buffer: copy it if you need it after returning. */
    uint64_t           num[CMD_MAX_ARGS];   /* num[i] = value of argument i when its type is 'num' */
};

struct sym_cmd {
    const char *name;
    const char *params;                     /* spec, see above */
    const char *help;                       /* one line for `help` */
    int (*fn)(const struct cmd_args *a);    /* return 0 on success, non-zero on failure */
};

/* The command name is the id, stringified: SYM_COMMAND(echo, ...) is "echo". */
#define SYM_COMMAND(id, params_, help_, fn_)                                \
    static const struct sym_cmd __sym_cmd_##id                              \
        __attribute__((used, section(".symbiote_cmd"), aligned(8))) =       \
        { #id, (params_), (help_), (fn_) }

/* Exit codes the dispatcher itself produces. A handler may also return
 * CMD_USAGE to say "you called me wrongly": the usage line is printed for it. */
enum { CMD_OK = 0, CMD_USAGE = 2, CMD_NOTFOUND = 127 };

/* Run a tokenized line: argv[0] is the command name.
 * Prints its own errors (not found, bad arguments, handler failure) and returns the exit code. */
int cmd_dispatch(int argc, char **argv);

const struct sym_cmd *cmd_find(const char *name);
size_t cmd_count(void);
const struct sym_cmd *cmd_at(size_t i);

/* "usage: name <params>" */
void cmd_print_usage(const struct sym_cmd *c);

/* Name of the command running right now, or NULL.
 * The panic screen prints this next to ext_current(), so a crash tells you which command did it. */
const char *cmd_current(void);

/* Check every registered command (duplicate names, malformed specs).
 * Prints each problem and returns how many there were; 0 means all good. */
int cmd_selfcheck(void);

#endif
