#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <limits.h>
#include <lib/mem.h>
#include <lib/string.h>
#include <core/arch/cpu.h>
#include <core/arch/heap.h>
#include <core/dev/vfs.h>
#include <core/bus.h>
#include <core/ext.h>
#include <core/klog.h>
#include <core/panic.h>


/*  global lock
 *
 * The filesystem is accessed from shell context, but a future driver or interrupt handler will want the same tree.
 * The lock is a plain spinlock taken with interrupts off:
 * an IRQ handler that calls into the VFS cannot arrive while the interrupted context holds the lock.
 * On UP this is a couple of instructions;
 * on SMP it serializes the whole tree, which is the right granularity until a per-node lock is justified. */
static volatile int vfs_lock;

/* node model */

struct vfs_node {
    enum vfs_type type;
    char name[VFS_NAME_MAX + 1];

    uint8_t *data;
    uint64_t size;

    struct vfs_node *children;
    struct vfs_node *next;
};

static struct vfs_node *root;

struct vfs_file {
    struct vfs_node *node;
    uint64_t pos;
    int flags;
};


static uint64_t vfs_acquire(void) {
    uint64_t flags = cpu_irq_save();
    while (__atomic_test_and_set(&vfs_lock, __ATOMIC_ACQUIRE))
        ;
    return flags;
}

static void vfs_release(uint64_t flags) {
    __atomic_clear(&vfs_lock, __ATOMIC_RELEASE);
    cpu_irq_restore(flags);
}

/* path handling */

static bool path_normalize(const char *path, char *out) {
    if (!path || !*path)
        return false;

    const char *comps[VFS_PATH_MAX / 2];
    size_t ncomp = 0;

    const char *p = path;
    while (*p) {
        while (*p == '/')
            p++;
        if (!*p)
            break;

        const char *start = p;
        while (*p && *p != '/')
            p++;
        size_t len = (size_t)(p - start);

        if (len == 1 && start[0] == '.')
            continue;
        if (len == 2 && start[0] == '.' && start[1] == '.') {
            if (ncomp == 0)
                return false;
            ncomp--;
            continue;
        }
        if (len > VFS_NAME_MAX)
            return false;
        if (ncomp >= sizeof comps / sizeof comps[0])
            return false;
        comps[ncomp++] = start;
    }

    char *o = out;
    *o++ = '/';
    for (size_t i = 0; i < ncomp; i++) {
        const char *c = comps[i];
        size_t clen = 0;
        while (c[clen] && c[clen] != '/')
            clen++;

        /* Pre-check bounds before writing component to prevent any overflow */
        if ((size_t)(o - out) + clen + 1 >= VFS_PATH_MAX)
            return false;

        for (size_t j = 0; j < clen; j++)
            *o++ = *c++;

        if (i + 1 < ncomp)
            *o++ = '/';
    }
    *o = '\0';
    return true;
}

static struct vfs_node *path_lookup(const char *path, struct vfs_node **parent_out) {
    if (path[0] != '/')
        return NULL;
    if (path[1] == '\0') {
        if (parent_out) *parent_out = NULL;
        return root;
    }

    struct vfs_node *cur = root;
    const char *p = path + 1;

    for (;;) {
        const char *start = p;
        while (*p && *p != '/')
            p++;
        size_t len = (size_t)(p - start);

        if (cur->type != VFS_DIR)
            return NULL;

        struct vfs_node *child = NULL;
        for (struct vfs_node *c = cur->children; c; c = c->next) {
            if (strlen(c->name) == len && memcmp(c->name, start, len) == 0) {
                child = c;
                break;
            }
        }
        if (!child) {
            if (parent_out && *p == '\0')
                *parent_out = cur;
            return NULL;
        }

        if (*p == '\0') {
            if (parent_out) *parent_out = cur;
            return child;
        }
        p++;
        cur = child;
    }
}

static struct vfs_node *node_new(enum vfs_type type, const char *name, size_t name_len) {
    if (name_len > VFS_NAME_MAX)
        return NULL;
    struct vfs_node *n = kzalloc(sizeof *n);
    if (!n)
        return NULL;
    n->type = type;
    memcpy(n->name, name, name_len);
    n->name[name_len] = '\0';
    return n;
}

static void node_free_recursive(struct vfs_node *n) {
    if (!n)
        return;
    if (n->type == VFS_DIR) {
        struct vfs_node *c = n->children;
        while (c) {
            struct vfs_node *next = c->next;
            node_free_recursive(c);
            c = next;
        }
    }
    kfree(n->data);
    kfree(n);
}

struct vfs_file *vfs_open(const char *path, int flags) {
    char norm[VFS_PATH_MAX];
    if (!path_normalize(path, norm))
        return NULL;

    uint64_t lock = vfs_acquire();

    struct vfs_node *parent = NULL;
    struct vfs_node *node = path_lookup(norm, &parent);

    bool created = false;

    if (node) {
        if (node->type != VFS_FILE) {
            vfs_release(lock);
            return NULL;
        }
        if (flags & VFS_O_TRUNC) {
            kfree(node->data);
            node->data = NULL;
            node->size = 0;
        }
    } else {
        if (!(flags & VFS_O_CREAT) || !parent) {
            vfs_release(lock);
            return NULL;
        }
        const char *base = strrchr(norm, '/');
        base = base ? base + 1 : norm;
        if (*base == '\0') {
            vfs_release(lock);
            return NULL;
        }
        node = node_new(VFS_FILE, base, strlen(base));
        if (!node) {
            vfs_release(lock);
            return NULL;
        }
        created = true;
    }

    struct vfs_file *f = kzalloc(sizeof *f);
    if (!f) {
        if (created)
            node_free_recursive(node);
        vfs_release(lock);
        return NULL;
    }

    if (created) {
        node->next = parent->children;
        parent->children = node;
    }

    f->node = node;
    f->flags = flags;
    if (flags & VFS_O_APPEND)
        f->pos = node->size;

    vfs_release(lock);
    return f;
}

void vfs_close(struct vfs_file *f) {
    kfree(f);
}

long vfs_read(struct vfs_file *f, void *buf, size_t n) {
    if (!f || !buf)
        return -1;
    if (f->flags == VFS_O_WRONLY)
        return -1;

    uint64_t lock = vfs_acquire();
    long rv;
    if (f->pos >= f->node->size) {
        rv = 0;
    } else {
        uint64_t avail = f->node->size - f->pos;
        if (n > avail)
            n = (size_t)avail;
        memcpy(buf, f->node->data + f->pos, n);
        f->pos += n;
        rv = (long)n;
    }
    vfs_release(lock);
    return rv;
}

long vfs_write(struct vfs_file *f, const void *buf, size_t n) {
    if (!f || !buf)
        return -1;
    if (f->flags == VFS_O_RDONLY)
        return -1;

    uint64_t lock = vfs_acquire();
    long rv = -1;

    if (f->pos + n < f->pos)
        goto out;

    uint64_t end = f->pos + n;
    if (end > SIZE_MAX)
        goto out;

    if (end > f->node->size) {
        uint8_t *fresh = krealloc(f->node->data, (size_t)end);
        if (!fresh)
            goto out;
        f->node->data = fresh;
        f->node->size = end;
    }

    memcpy(f->node->data + f->pos, buf, n);
    f->pos += n;
    rv = (long)n;

out:
    vfs_release(lock);
    return rv;
}

long vfs_lseek(struct vfs_file *f, long off, int whence) {
    if (!f)
        return -1;

    uint64_t lock = vfs_acquire();

    uint64_t base;
    switch (whence) {
    case 0: base = 0;                 break;   /* SET */
    case 1: base = f->pos;              break;   /* CUR */
    case 2: base = f->node->size;       break;   /* END */
    default: vfs_release(lock); return -1;
    }

    uint64_t np;
    if (off < 0) {
        uint64_t mag = (uint64_t)(-(off + 1)) + 1;
        if (mag > base) {
            vfs_release(lock);
            return -1;
        }
        np = base - mag;
    } else {
        if ((uint64_t)off > UINT64_MAX - base) {
            vfs_release(lock);
            return -1;
        }
        np = base + (uint64_t)off;
    }

    if (np > f->node->size) {
        vfs_release(lock);
        return -1;
    }
    f->pos = np;

    if (np > (uint64_t)LONG_MAX) {
        vfs_release(lock);
        return -1;
    }
    long rv = (long)np;
    vfs_release(lock);
    return rv;
}

int vfs_mkdir(const char *path) {
    char norm[VFS_PATH_MAX];
    if (!path_normalize(path, norm))
        return -1;

    uint64_t lock = vfs_acquire();

    struct vfs_node *parent = NULL;
    struct vfs_node *existing = path_lookup(norm, &parent);
    if (existing) {
        vfs_release(lock);
        return -1;      /* already exists */
    }
    if (!parent) {
        vfs_release(lock);
        return -1;
    }

    const char *base = strrchr(norm, '/');
    base = base ? base + 1 : norm;
    if (*base == '\0') {
        vfs_release(lock);
        return -1;
    }

    struct vfs_node *n = node_new(VFS_DIR, base, strlen(base));
    if (!n) {
        vfs_release(lock);
        return -1;
    }
    n->next = parent->children;
    parent->children = n;

    vfs_release(lock);
    return 0;
}

int vfs_unlink(const char *path) {
    char norm[VFS_PATH_MAX];
    if (!path_normalize(path, norm))
        return -1;

    uint64_t lock = vfs_acquire();

    struct vfs_node *parent = NULL;
    struct vfs_node *node = path_lookup(norm, &parent);
    if (!node || !parent) {
        vfs_release(lock);
        return -1;      /* not found, or attempting to remove root */
    }

    /* Optional safety: refuse to unlink non-empty directories unless intended */
    if (node->type == VFS_DIR && node->children != NULL) {
        vfs_release(lock);
        return -1;      /* directory not empty */
    }

    struct vfs_node **pp = &parent->children;
    while (*pp && *pp != node)
        pp = &(*pp)->next;
    if (*pp)
        *pp = node->next;

    vfs_release(lock);

    node_free_recursive(node);
    return 0;
}

int vfs_stat(const char *path, struct vfs_stat *out) {
    if (!out)
        return -1;
    char norm[VFS_PATH_MAX];
    if (!path_normalize(path, norm))
        return -1;

    uint64_t lock = vfs_acquire();
    struct vfs_node *n = path_lookup(norm, NULL);
    if (!n) {
        vfs_release(lock);
        return -1;
    }
    out->type = n->type;
    out->size = n->type == VFS_FILE ? n->size : 0;
    vfs_release(lock);
    return 0;
}

bool vfs_readdir(const char *path, size_t *iter, char *name) {
    if (!iter || !name)
        return false;
    char norm[VFS_PATH_MAX];
    if (!path_normalize(path, norm))
        return false;

    uint64_t lock = vfs_acquire();

    struct vfs_node *n = path_lookup(norm, NULL);
    if (!n || n->type != VFS_DIR) {
        vfs_release(lock);
        return false;
    }

    struct vfs_node *c = n->children;
    for (size_t i = 0; c && i < *iter; i++)
        c = c->next;
    if (!c) {
        vfs_release(lock);
        return false;
    }

    strncpy(name, c->name, VFS_NAME_MAX);
    name[VFS_NAME_MAX] = '\0';
    (*iter)++;

    vfs_release(lock);
    return true;
}


/* bus back-end */
static const struct vfs_ops vfs_impl = {
    .open    = vfs_open,
    .close   = vfs_close,
    .read    = vfs_read,
    .write   = vfs_write,
    .lseek   = vfs_lseek,
    .mkdir   = vfs_mkdir,
    .unlink  = vfs_unlink,
    .stat    = vfs_stat,
    .readdir = vfs_readdir,
};

/* init */

void vfs_init(void) {
    uint64_t flags = vfs_acquire();
    root = node_new(VFS_DIR, "", 0);
    vfs_release(flags);

    if (!root)
        PANIC("vfs init: cannot allocate root");
    klog("→ vfs_init() root mounted (flags=%lu)\n", flags);

    // success, announce on bus
    struct bus_device dev = {
        .name         = "vfs",
        .class        = BUS_CLASS_VFS,
        .version      = 1,
        .ops          = &vfs_impl,
        .private_data = NULL,
        .owner_id     = 4,
    };
    bus_register(&dev);
    bus_test("vfs");
}
