#ifndef CORE_EXT_H
#define CORE_EXT_H

#include <stddef.h>
#include <core/version.h>

/* codebase: 0.1.2 */

/* An extension is one descriptor in the ".symbiote_ext" linker section.
 * The core never needs a list of extensions: the linker gathers them.
 * To add one, drop a .c file in src/ext/ and name it in EXTENSIONS.
 * To remove one, take it out of EXTENSIONS and its code is not in the image at all. */
struct sym_ext {
    const char *name;
    int (*init)(void);      /* return 0 on success, non-zero on failure */
    int prio;               /* lower runs first */
};

// moved to config.h
/* Init order. Pick the level that matches what your extension provides.
enum ext_prio {
    EXT_PRIO_ALWAYS_FIRST   = 0,   // Always first in-line
    EXT_PRIO_CONSOLE        = 10,  // Output backends
    EXT_PRIO_DRIVER         = 10,  // Hardware drivers
    EXT_PRIO_FS             = 20,  // Filesystems
    EXT_PRIO_SERVICE        = 30,  // Shell applications
    EXT_PRIO_APPLET         = 40,  // Extension applications
};
*/
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
