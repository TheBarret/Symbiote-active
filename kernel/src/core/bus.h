#ifndef CORE_BUS_H
#define CORE_BUS_H

#include <stddef.h>
#include <stdint.h>
#include <core/version.h>   /* pulls in config.h: BUS_MAX_DEVICES */

/* Software bus: a name -> ops registry, linear with the core.
 *
 * Not an extension, not a layer above the core. Always compiled in.
 * Every core module that wants to be discoverable includes this header and calls bus_register() from its own init.
 *
 * This is a registry, not a message bus and not a uniform vtable.
 * Each device class has its own ops struct (console_ops, vfs_ops, ...);
 * the bus stores a void* plus a class tag, and typed finders cast on return.
 * There is deliberately no read/write/ioctl signature here:
 * a timer is not a block device, and pretending otherwise buys nothing.
 *
 * The bus exists for two consumers:
 *   1. The script VM, which cannot #include kernel headers and must discover devices at runtime.
 *   2. A future loadable-driver path, where the device set is not known at link time.

 * Statically-linked C extensions and core modules should keep calling their module APIs directly;
 * routing through the bus buys them nothing.
 *
 * Backed by a static array, so bus_init() runs before heap_init().
 * Registration happens in two waves:
 *   wave 1  core modules, in kmain order (serial, timer, vfs, ...)
 *   wave 2  extensions, in ext_init_all() priority order

 * Both waves only call bus_register(); neither needs the heap.
 *
 * Single-threaded boot assumption:
 * registration happens during kmain and ext_init_all(), lookup happens afterwards. No locking.
 */

enum bus_class {
    BUS_CLASS_NONE    = 0,   /* null */
    BUS_CLASS_CONSOLE = 1,   /* serial.c, console.c */
    BUS_CLASS_TIMER   = 2,   /* interrupts, timer.c */
    BUS_CLASS_VFS     = 3,   /* virtual filesystem, vfs.c */
    BUS_CLASS_LOG     = 4,   /* kerner logger, klog.c */
    BUS_CLASS_INPUT   = 5,   /* keyboard input, keyboard.c */
    BUS_CLASS_ENV     = 6,   /* environment, env.c */
    BUS_CLASS_USER    = 7,   /* user defined class */
    /* grow as devices appear; keep values stable once assigned */
};

struct bus_device {
    const char *name;         /* unique, e.g. "console", "timer", "vfs" */
    enum bus_class class;
    uint16_t version;         /* module revision, exposed to scripts */
    //void    *ops;             /* struct <class>_ops *, cast on lookup */
    const void *ops;
    void    *private_data;    /* backend state, opaque to the bus */
};

/* Zero the table. First thing after serial_init() in kmain,
 * before gdt_init(). Must run before any bus_register(). */
void bus_init(void);

/* Register a device. The struct is copied into the table; the caller
 * does not need to keep it alive (but ops and private_data must be).
 *
 * Returns 0, or -1 if:
 *   - the table is full (BUS_MAX_DEVICES)
 *   - dev or dev->name is NULL
 *   - dev->class is BUS_CLASS_NONE
 *   - the name is already registered (first registrant wins)
 *
 * Registration only during kmain and ext_init_all(). Calling this
 * after shell_run() is a bug; the table has no lock. */
int bus_register(const struct bus_device *dev);

/* little test if find-by-name results in success */
int bus_test(const char *name);

/* Generic lookup. Returns NULL if not found.
 * bus_find_class(class, 0) is the first device of that class,
 * (class, 1) the second, and so on. */
const struct bus_device *bus_find(const char *name);
const struct bus_device *bus_find_class(enum bus_class class, size_t index);

/* Typed finders */
const struct console_ops *bus_console(void);
const struct timer_ops *bus_timer(void);
const struct env_ops *bus_env(void);
const struct vfs_ops *bus_vfs(void);

/* Number of registered devices, and iteration for `lsbus`. */
size_t bus_count(void);
const struct bus_device *bus_at(size_t index);

#endif
