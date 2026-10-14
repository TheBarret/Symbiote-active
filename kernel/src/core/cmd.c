#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <core/cmd.h>
#include <core/kprintf.h>
#include <lib/mem.h>
#include <lib/string.h>

/* Provided by the linker script (linker-scripts/x86_64.lds). */
extern const struct sym_cmd __symbiote_cmd_start[];
extern const struct sym_cmd __symbiote_cmd_end[];

static const char *current;

const char *cmd_current(void) { return current; }

size_t cmd_count(void) {
    return (size_t)(__symbiote_cmd_end - __symbiote_cmd_start);
}

const struct sym_cmd *cmd_at(size_t i) {
    return i < cmd_count() ? &__symbiote_cmd_start[i] : NULL;
}

const struct sym_cmd *cmd_find(const char *name) {
    /* A linear scan is the right lookup: a few dozen strcmp() calls cost
     * nothing next to a human typing, and the table stays a plain array. */
    for (const struct sym_cmd *c = __symbiote_cmd_start; c < __symbiote_cmd_end; c++)
        if (strcmp(c->name, name) == 0)
            return c;
    return NULL;
}

void cmd_print_usage(const struct sym_cmd *c) {
    if (c->params[0])
        kprintf("usage: %s %s\n", c->name, c->params);
    else
        kprintf("usage: %s   (takes no arguments)\n", c->name);
}

/*  parameter spec  */

#define TOK_MAX 24      /* longest single parameter, e.g. "[iterations:num]" */

struct param {
    char tok[TOK_MAX];  /* the parameter exactly as written, for messages */
    enum { P_STR, P_NUM } type;
    bool optional;
    bool rest;
};

/* Parse a spec string into out[]. Returns NULL on success, or a short reason
 * the spec is malformed (that is a bug in the command, not in the user). */
static const char *parse_spec(const char *s, struct param *out, int *count) {
    int n = 0;
    bool seen_optional = false;

    while (*s) {
        /* Skip one or more separating spaces. */
        s += strspn(s, " ");

        if (*s == '\0')
            break;
        if (n == CMD_MAX_ARGS)
            return "too many parameters";
        if (n > 0 && out[n - 1].rest)
            return "a '...' parameter must be the last one";

        char open = *s, close;
        if (open == '<')
            close = '>';
        else if (open == '[')
            close = ']';
        else
            return "parameter must look like <name:type> or [name:type]";

        const char *end = s + 1;
        while (*end && *end != close)
            end++;
        if (*end != close)
            return "missing closing bracket";

        size_t len = (size_t)(end - s) + 1;         /* includes both brackets */
        if (len >= TOK_MAX)
            return "parameter is too long";
        for (size_t i = 0; i < len; i++)
            out[n].tok[i] = s[i];
        out[n].tok[len] = '\0';

        const char *colon = s + 1;
        while (colon < end && *colon != ':')
            colon++;
        if (colon == s + 1 || colon == end)
            return "parameter needs a name and a type (name:type)";

        const char *type = colon + 1;
        size_t tlen = (size_t)(end - type);
        bool rest = false;
        if (tlen >= 3 && memcmp(end - 3, "...", 3) == 0) {
            rest = true;
            tlen -= 3;
        }
        if (tlen == 3 && memcmp(type, "num", 3) == 0)
            out[n].type = P_NUM;
        else if (tlen == 3 && memcmp(type, "str", 3) == 0)
            out[n].type = P_STR;
        else
            return "unknown type (use num or str)";

        bool optional = (open == '[');
        if (!optional && seen_optional)
            return "required parameter after an optional one";
        seen_optional |= optional;
        out[n].optional = optional;
        out[n].rest = rest;

        s = end + 1;
        if (*s && *s != ' ')
            return "parameters must be separated by spaces";
        n++;
    }
    *count = n;
    return NULL;
}

/*  numbers --- */

enum num_status { NUM_OK, NUM_BAD, NUM_OVERFLOW };

/* Decimal or 0x-prefixed hex, unsigned 64-bit, whole string consumed.
 * This is the command dispatcher's notion of a number argument.
 * Distinct from strtoull() in /lib, which accepts a trailing suffix and
 * reports where parsing stopped; command arguments must be exact. */
static enum num_status parse_num(const char *s, uint64_t *out) {
    uint64_t v;
    if (!kstrtoull(s, &v))
        return NUM_BAD;
    *out = v;
    return NUM_OK;
}

/*  dispatch -- */

int cmd_dispatch(int argc, char **argv) {
    if (argc <= 0)
        return CMD_OK;

    const struct sym_cmd *c = cmd_find(argv[0]);
    if (!c) {
        kprintf("%s: command not found. Try 'help'.\n", argv[0]);
        return CMD_NOTFOUND;
    }

    struct param p[CMD_MAX_ARGS];
    int np = 0;
    const char *err = parse_spec(c->params, p, &np);
    if (err) {
        kprintf("%s: broken parameter spec \"%s\": %s\n", c->name, c->params, err);
        return CMD_USAGE;
    }

    int nargs = argc - 1;
    int required = 0;
    for (int i = 0; i < np; i++)
        if (!p[i].optional)
            required++;
    bool has_rest = np > 0 && p[np - 1].rest;

    if (nargs < required) {
        /* Required parameters come first, so p[nargs] is the first missing one. */
        kprintf("%s: missing %s\n", c->name, p[nargs].tok);
        cmd_print_usage(c);
        return CMD_USAGE;
    }
    if ((!has_rest && nargs > np) || nargs > CMD_MAX_ARGS) {
        if (np == 0)
            kprintf("%s: takes no arguments (got %d)\n", c->name, nargs);
        else
            kprintf("%s: too many arguments (takes at most %d, got %d)\n",
                    c->name, has_rest ? CMD_MAX_ARGS : np, nargs);
        cmd_print_usage(c);
        return CMD_USAGE;
    }

    struct cmd_args a = {
        .argc = nargs,
        .argv = (const char *const *)(argv + 1),
    };

    for (int i = 0; i < nargs; i++) {
        const struct param *pp = &p[i < np ? i : np - 1];   /* extras use the rest param */
        if (pp->type != P_NUM)
            continue;
        switch (parse_num(argv[i + 1], &a.num[i])) {
        case NUM_OK:
            break;
        case NUM_BAD:
            kprintf("%s: argument %d %s: '%s' is not a number\n",
                    c->name, i + 1, pp->tok, argv[i + 1]);
            cmd_print_usage(c);
            return CMD_USAGE;
        case NUM_OVERFLOW:
            kprintf("%s: argument %d %s: '%s' does not fit in 64 bits\n",
                    c->name, i + 1, pp->tok, argv[i + 1]);
            cmd_print_usage(c);
            return CMD_USAGE;
        }
    }

    current = c->name;
    int rc = c->fn(&a);
    current = NULL;

    if (rc == CMD_USAGE)
        cmd_print_usage(c);
    else if (rc != 0)
        kprintf("%s: exit %d\n", c->name, rc);
    return rc;
}

/*  self check  */

int cmd_selfcheck(void) {
    int problems = 0;
    size_t n = cmd_count();

    for (size_t i = 0; i < n; i++) {
        const struct sym_cmd *c = cmd_at(i);

        if (!c->name || !c->name[0] || !c->params || !c->help || !c->fn) {
            kprintf("Warning: entry %zu is incomplete (missing name, params, help or handler)\n", i);
            problems++;
            continue;
        }

        /* Command names must be non-empty and contain no spaces: the shell
         * tokenizes on spaces, so a name with one could never be typed. */
        if (strcspn(c->name, " \t") != strlen(c->name)) {
            kprintf("Warning: '%s' has whitespace in its name\n", c->name);
            problems++;
        }

        struct param p[CMD_MAX_ARGS];
        int np;
        const char *err = parse_spec(c->params, p, &np);
        if (err) {
            kprintf("Warning: '%s' has a broken spec \"%s\": %s\n", c->name, c->params, err);
            problems++;
        }

        for (size_t j = 0; j < i; j++) {
            const struct sym_cmd *d = cmd_at(j);
            if (d->name && strcmp(d->name, c->name) == 0) {
                kprintf("Warning: duplicate command '%s'\n", c->name);
                problems++;
                break;
            }
        }
    }
    return problems;
}
