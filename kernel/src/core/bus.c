#include "klog.h"
#include <stddef.h>
#include <lib/string.h>
#include <core/console.h>
#include <core/klog.h>
#include <core/timer.h>
#include <core/vfs.h>
#include <core/bus.h>

/* Software bus: a name -> ops registry, linear with the core.
 *
 * Backed by a static array so bus_init() can run before heap_init().
 * See core/bus.h for the model and the registration contract.
 *
 * Single-threaded boot assumption throughout: registration happens
 * during kmain and ext_init_all(), lookup happens afterwards.
 */

static struct bus_device bus_table[BUS_MAX_DEVICES];
static size_t bus_used;

void bus_init(void) {
    for (size_t i = 0; i < BUS_MAX_DEVICES; i++) {
        bus_table[i].name = NULL;
        bus_table[i].class = BUS_CLASS_NONE;
        bus_table[i].version = 0;
        bus_table[i].ops = NULL;
        bus_table[i].private_data = NULL;
    }
    bus_used = 0;
    klog_bus("Initialized (max=%u)\n", BUS_MAX_DEVICES);
}

int bus_register(const struct bus_device *dev) {
    if (dev == NULL || dev->name == NULL || dev->name[0] == '\0') {
        return -1;
    }
    if (dev->class == BUS_CLASS_NONE) {
        return -1;
    }
    if (bus_used >= BUS_MAX_DEVICES) {
        return -1;
    }

    /* First registrant wins. A duplicate name is a bug in the caller,
     * not something to silently paper over. */
    for (size_t i = 0; i < bus_used; i++) {
        if (strcmp(bus_table[i].name, dev->name) == 0) {
            return -1;
        }
    }

    bus_table[bus_used] = *dev;
    bus_used++;
    //klog("bus_register(): name=%s, class=%u\n", dev->name, dev->class);
    return 0;
}

int bus_test(const char *name) {
    const struct bus_device *d = bus_find(name);
    const struct bus_device *miss = bus_find("no-such-device");
    const struct console_ops *c = bus_console();
    klog_bus("→ Found %s registry: count=%zu, class=%u ops=%s write=%s available=%s\n",
              d ? d->name : "(empty)",
              bus_count(),
              d ? (unsigned)d->class : 0,
              (d && d->ops) ? "true" : "false",
              (c && c->write == console_write) ? "true" : "false",
              miss ? "false" : "true");
    return 0;
}


const struct bus_device *bus_find(const char *name) {
    if (name == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < bus_used; i++) {
        if (strcmp(bus_table[i].name, name) == 0) {
            return &bus_table[i];
        }
    }
    return NULL;
}

const struct bus_device *bus_find_class(enum bus_class class, size_t index) {
    if (class == BUS_CLASS_NONE) {
        return NULL;
    }
    size_t seen = 0;
    for (size_t i = 0; i < bus_used; i++) {
        if (bus_table[i].class != class) {
            continue;
        }
        if (seen == index) {
            return &bus_table[i];
        }
        seen++;
    }
    return NULL;
}

/* typed finders */

const struct console_ops *bus_console(void) {
    const struct bus_device *d = bus_find_class(BUS_CLASS_CONSOLE, 0);
    return (d != NULL) ? (const struct console_ops *)d->ops : NULL;
}


const struct vfs_ops *bus_vfs(void) {
    const struct bus_device *d = bus_find_class(BUS_CLASS_VFS, 0);
    return (d != NULL) ? (const struct vfs_ops *)d->ops : NULL;
}

const struct timer_ops *bus_timer(void) {
    const struct bus_device *d = bus_find_class(BUS_CLASS_TIMER, 0);
    return (d != NULL) ? (const struct timer_ops *)d->ops : NULL;
}

//const struct klog_ops *bus_klog(void) {
//    const struct bus_device *d = bus_find_class(BUS_CLASS_LOG, 0);
//    return (d != NULL) ? (const struct klog_ops *)d->ops : NULL;
//}

size_t bus_count(void) {
    return bus_used;
}

const struct bus_device *bus_at(size_t index) {
    if (index >= bus_used) {
        return NULL;
    }
    return &bus_table[index];
}
