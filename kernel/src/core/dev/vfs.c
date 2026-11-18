#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <limits.h>
#include <lib/mem.h>
#include <lib/string.h>
#include <core/arch/cpu.h>
#include <core/arch/heap.h>
#include <core/dev/vfs.h>
#include <core/ext.h>
#include <core/klog.h>
#include <core/panic.h>

/* codebase: 0.1.2 */

/* Tunables (override from config.h or CPPFLAGS)
 *
 * VFS_FILE_MAX   hard cap on one file's size. Growth beyond it fails the write with -1.
 * VFS_GROW_MIN   first allocation size for a file; later growth doubles.
 */
#ifndef VFS_FILE_MAX
#define VFS_FILE_MAX   ((uint64_t)1 << 20)
#endif
#ifndef VFS_GROW_MIN
#define VFS_GROW_MIN   64u
#endif

/* global lock
 *
 * Plain spinlock taken with interrupts off.
 * The kernel is single-core, so a held lock seen by the same context can only mean re-entry (a bug) and would spin forever.
 * Debug builds panic instead of hanging.
 */
static volatile uint8_t vfs_lock;

/* node model
 *
 * Data invariant (checked under SYM_MEMDEBUG):
 *     size <= cap,  (data == NULL) <=> (cap == 0),  and every byte in [size, cap) is ZERO.
 * The zero tail is what makes a write past EOF safe: the gap is already zero,
 * so stale heap contents can never become readable file data. */
struct vfs_node {
    enum vfs_type type;
    uint16_t      name_len;
    char          name[VFS_NAME_MAX + 1];

    uint8_t      *data;
    uint64_t      size;
    uint64_t      cap;

    uint32_t      refs;             /* open handles; unlink defers free until 0 */
    bool          dead;             /* unlinked from tree; free when refs == 0 */

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
#ifdef SYM_MEMDEBUG
    if (__atomic_test_and_set(&vfs_lock, __ATOMIC_ACQUIRE))
        PANIC("vfs: lock re-entered");
#else
    while (__atomic_test_and_set(&vfs_lock, __ATOMIC_ACQUIRE))
        ;
#endif
    return flags;
}

static void vfs_release(uint64_t flags) {
    __atomic_clear(&vfs_lock, __ATOMIC_RELEASE);
    cpu_irq_restore(flags);
}

#ifdef SYM_MEMDEBUG
static void node_check(const struct vfs_node *n) {
    if (n->size > n->cap)                   PANIC("vfs: size > cap");
    if ((n->data == NULL) != (n->cap == 0)) PANIC("vfs: data/cap mismatch");
    for (uint64_t i = n->size; i < n->cap; i++)
        if (n->data[i] != 0)                PANIC("vfs: dirty tail");
}
#else
#define node_check(n) ((void)0)
#endif

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

        /* Pre-check bounds before writing the component. */
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

/* `path` must already be normalized (absolute). On a miss where the parent
 * directory exists, *parent_out is set to it so callers can create the leaf. */
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
            if (c->name_len == len && memcmp(c->name, start, len) == 0) {
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

/* node lifecycle */

static struct vfs_node *node_new(enum vfs_type type, const char *name, size_t name_len) {
    if (name_len > VFS_NAME_MAX)
        return NULL;
    struct vfs_node *n = kzalloc(sizeof *n);
    if (!n)
        return NULL;
    n->type = type;
    n->name_len = (uint16_t)name_len;
    memcpy(n->name, name, name_len);
    n->name[name_len] = '\0';
    return n;
}

/* Create the leaf named by the last component of `norm` and link it under `parent`.
 * Shared by open(O_CREAT) and mkdir so both follow one path. NULL on empty leaf or OOM. */
static struct vfs_node *node_create(struct vfs_node *parent, const char *norm,
                                    enum vfs_type type) {
    const char *base = strrchr(norm, '/');
    base = base ? base + 1 : norm;
    if (*base == '\0')
        return NULL;

    struct vfs_node *n = node_new(type, base, strlen(base));
    if (!n)
        return NULL;
    n->next = parent->children;
    parent->children = n;
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

static void node_truncate(struct vfs_node *n) {
    kfree(n->data);
    n->data = NULL;
    n->size = 0;
    n->cap = 0;
}

/* Make room for `need` bytes. Doubles capacity (clamped to VFS_FILE_MAX) so appends are amortized
 * instead of reallocating to the exact size every time; under memory pressure it retries with an
 * exact fit. New capacity is zero-filled to keep the data invariant. On failure the node is untouched. */
static bool node_reserve(struct vfs_node *n, uint64_t need) {
    if (need <= n->cap)
        return true;
    if (need > VFS_FILE_MAX)
        return false;

    uint64_t cap = n->cap ? n->cap : VFS_GROW_MIN;
    while (cap < need) {
        if (cap > UINT64_MAX / 2) { cap = need; break; }
        cap *= 2;
    }
    if (cap > VFS_FILE_MAX)
        cap = VFS_FILE_MAX;
    if (cap > (uint64_t)SIZE_MAX)
        return false;

    uint8_t *fresh = krealloc(n->data, (size_t)cap);
    if (!fresh && cap != need) {
        cap = need;                       /* fall back to an exact fit */
        fresh = krealloc(n->data, (size_t)cap);
    }
    if (!fresh)
        return false;                     /* krealloc leaves the original intact */

    memset(fresh + n->cap, 0, (size_t)(cap - n->cap));
    n->data = fresh;
    n->cap = cap;
    return true;
}

/* files */

struct vfs_file *vfs_open(const char *path, int flags) {
    char norm[VFS_PATH_MAX];
    if (!path_normalize(path, norm))
        return NULL;

    /* Allocate the handle first so no later failure can leave a side effect
     * (previously O_TRUNC had already destroyed data when this allocation failed). */
    struct vfs_file *f = kzalloc(sizeof *f);
    if (!f)
        return NULL;

    uint64_t lock = vfs_acquire();

    struct vfs_node *parent = NULL;
    struct vfs_node *node = path_lookup(norm, &parent);

    if (node) {
        if (node->type != VFS_FILE)
            goto fail;
        /* Truncation only on an open that asked for write access. */
        if ((flags & VFS_O_TRUNC) && (flags & (VFS_O_WRONLY | VFS_O_RDWR)))
            node_truncate(node);
    } else {
        if (!(flags & VFS_O_CREAT) || !parent)
            goto fail;
        node = node_create(parent, norm, VFS_FILE);
        if (!node)
            goto fail;
    }

    f->node = node;
    f->flags = flags;
    f->pos = (flags & VFS_O_APPEND) ? node->size : 0;
    node->refs++;

    vfs_release(lock);
    return f;

fail:
    vfs_release(lock);
    kfree(f);
    return NULL;
}

void vfs_close(struct vfs_file *f) {
    if (!f)
        return;
    uint64_t lock = vfs_acquire();
    struct vfs_node *n = f->node;
    if (n) {
        if (n->refs > 0)
            n->refs--;
        if (n->dead && n->refs == 0)
            node_free_recursive(n);
    }
    vfs_release(lock);
    kfree(f);
}

long vfs_read(struct vfs_file *f, void *buf, size_t n) {
    if (!f || !buf)
        return -1;
    /* RDONLY = 0; WRONLY alone (possibly OR'd with CREAT/TRUNC/APPEND) denies read. */
    if ((f->flags & (VFS_O_WRONLY | VFS_O_RDWR)) == VFS_O_WRONLY)
        return -1;

    uint64_t lock = vfs_acquire();
    long rv = -1;
    if (!f->node)
        goto out;

    if (f->pos >= f->node->size || n == 0) {
        rv = 0;
    } else {
        uint64_t avail = f->node->size - f->pos;
        if (n > avail)
            n = (size_t)avail;
        memcpy(buf, f->node->data + f->pos, n);
        f->pos += n;
        rv = (long)n;
    }
out:
    vfs_release(lock);
    return rv;
}

long vfs_write(struct vfs_file *f, const void *buf, size_t n) {
    if (!f || !buf)
        return -1;
    /* Need an explicit write bit; bare RDONLY (0) or RDONLY|CREAT must fail. */
    if (!(f->flags & (VFS_O_WRONLY | VFS_O_RDWR)))
        return -1;
    if (n == 0)
        return 0;
    if (n > (size_t)LONG_MAX)       /* the return value would read as an error */
        return -1;

    uint64_t lock = vfs_acquire();
    long rv = -1;

    struct vfs_node *node = f->node;
    if (!node)
        goto out;

    /* O_APPEND: every write lands at the current end, even if another handle grew the file. */
    uint64_t pos = (f->flags & VFS_O_APPEND) ? node->size : f->pos;
    if ((uint64_t)n > UINT64_MAX - pos)
        goto out;
    uint64_t end = pos + (uint64_t)n;

    if (end > node->size) {
        /* pos may be > size if another handle truncated; the gap is already zero (see invariant). */
        if (!node_reserve(node, end))
            goto out;
        node->size = end;
    }

    memcpy(node->data + pos, buf, n);
    f->pos = end;
    rv = (long)n;
    node_check(node);

out:
    vfs_release(lock);
    return rv;
}

long vfs_lseek(struct vfs_file *f, long off, int whence) {
    if (!f)
        return -1;

    uint64_t lock = vfs_acquire();
    long rv = -1;

    uint64_t base;
    switch (whence) {
    case 0: base = 0;                  break;   /* SET */
    case 1: base = f->pos;             break;   /* CUR */
    case 2: base = f->node->size;      break;   /* END */
    default: goto out;
    }

    uint64_t np;
    if (off < 0) {
        uint64_t mag = (uint64_t)(-(off + 1)) + 1;   /* safe for LONG_MIN */
        if (mag > base)
            goto out;
        np = base - mag;
    } else {
        if ((uint64_t)off > UINT64_MAX - base)
            goto out;
        np = base + (uint64_t)off;
    }

    /* Seeking past EOF is refused (unchanged behavior). */
    if (np > f->node->size || np > (uint64_t)LONG_MAX)
        goto out;

    f->pos = np;
    rv = (long)np;
out:
    vfs_release(lock);
    return rv;
}

/* directories */

int vfs_mkdir(const char *path) {
    char norm[VFS_PATH_MAX];
    if (!path_normalize(path, norm))
        return -1;

    uint64_t lock = vfs_acquire();

    struct vfs_node *parent = NULL;
    struct vfs_node *existing = path_lookup(norm, &parent);
    int rv = -1;

    if (!existing && parent && node_create(parent, norm, VFS_DIR))
        rv = 0;

    vfs_release(lock);
    return rv;
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

    if (node->type == VFS_DIR && node->children != NULL) {
        vfs_release(lock);
        return -1;      /* directory not empty */
    }

    struct vfs_node **pp = &parent->children;
    while (*pp && *pp != node)
        pp = &(*pp)->next;
    if (*pp)
        *pp = node->next;
    node->next = NULL;

    node->dead = true;
    if (node->refs == 0)
        node_free_recursive(node);

    vfs_release(lock);
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
    out->size = (n->type == VFS_FILE) ? n->size : 0;
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

    memcpy(name, c->name, (size_t)c->name_len + 1);   /* includes the NUL */
    (*iter)++;

    vfs_release(lock);
    return true;
}

/* init */

void vfs_init(void) {
    if (root != NULL) {
        klog_warning("vfs_init() called twice; ignoring\n");
        return;
    }
    root = node_new(VFS_DIR, "", 0);
    if (!root)
        PANIC("vfs init: cannot allocate root");
    klog("• VFS      [OK]\n");

}
