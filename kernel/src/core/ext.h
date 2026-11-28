#ifndef CORE_EXT_H
#define CORE_EXT_H

#include <stddef.h>
#include <core/version.h>

/* codebase: 0.2.0 */

/* An extension is one descriptor in the ".symbiote_ext" linker section.
 * The core never needs a list of extensions: the linker gathers them.
 * To add one, drop a .c file in src/ext/ and name it in EXTENSIONS.
 * To remove one, take it out of EXTENSIONS and its code is not in the image at all.
 *
 * Init order is by prio (enum ext_prio in config.h), lowest first.
 */
struct sym_ext {
    const char *name;
    int (*init)(void);      /* return 0 on success, non-zero on failure */
    int prio;               /* lower runs first */
};

#define SYM_EXTENSION(id, init_fn, prio_)                                   \
    static const struct sym_ext __sym_ext_##id                              \
        __attribute__((used, section(".symbiote_ext"), aligned(8))) =       \
        { #id, (init_fn), (prio_) }

/* Run every extension's init, lowest priority first. */
void ext_init_all(void);

/* Name of the extension whose init is running right now, or NULL.
 * The panic screen prints this: it tells you who to blame. */
const char *ext_current(void);

size_t ext_count(void);

#endif
