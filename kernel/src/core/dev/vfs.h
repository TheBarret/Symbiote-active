#ifndef CORE_VFS_H
#define CORE_VFS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <core/version.h>

/* Virtual file system.
 *
 * One interface, one implementation (ramfs) for now.
 * The point of the interface is that a future filesystem, read-only from a Limine module,
 * or a real disk-backed one, can be plugged in without the shell or any other caller knowing which one answered.
 *
 * Paths are absolute ("/foo/bar") or relative to the root if they lack a leading '/'. "." and ".." are recognized.
 * Multiple consecutive slashes are collapsed.
 *
 * Everything is byte-oriented: read/write move whole bytes, lseek sets the position.
 * There is no block layer; a future disk filesystem would put its own block cache inside its implementation.
 */

enum vfs_type {
    VFS_FILE,
    VFS_DIR,
};

enum vfs_open_flags {
    VFS_O_RDONLY = 0,           /* "no write bits set", not a distinct bit */
    VFS_O_WRONLY = 1 << 0,
    VFS_O_RDWR   = 1 << 1,
    VFS_O_CREAT  = 1 << 2,
    VFS_O_TRUNC  = 1 << 3,
    VFS_O_APPEND = 1 << 4,
};

/* An open file. The caller owns the struct and passes it back to read / write / close. Its contents are opaque.
 *
 * Unlink contract: vfs_unlink unhooks the node from the tree immediately.
 * Open handles keep the node alive (refcount); the node is freed on the
 * last vfs_close. Reads/writes on an unlinked node still see its data
 * until close; path lookup will not find it. */
struct vfs_file;

/* Node metadata, as returned by vfs_stat. */
struct vfs_stat {
    enum vfs_type type;
    uint64_t size;      /* for files: bytes; for dirs: 0 */
};

/*  lifecycle  */

void vfs_init(void);

/*  files  */

struct vfs_file *vfs_open(const char *path, int flags);
void             vfs_close(struct vfs_file *f);

/* Return the number of bytes read/written, or -1 on error. */
long vfs_read(struct vfs_file *f, void *buf, size_t n);
long vfs_write(struct vfs_file *f, const void *buf, size_t n);

/* Set the position. Returns the new position, or -1 on error.
 * Whence: 0 = SET, 1 = CUR, 2 = END. */
long vfs_lseek(struct vfs_file *f, long off, int whence);

/*  directories  */

int vfs_mkdir(const char *path);
int vfs_unlink(const char *path);
int vfs_stat(const char *path, struct vfs_stat *out);

/* List directory entries, one name at a time.
 * Set *iter to 0 on the first call; it is advanced internally.
 * Returns true and writes the next name to `name` (NUL-terminated).
 * The caller must provide a buffer of at least VFS_NAME_MAX + 1 bytes.
 * Returns false when there are no more entries. */
bool vfs_readdir(const char *path, size_t *iter, char *name);



/* bus back-end */
struct vfs_ops {
    struct vfs_file *(*open)(const char *path, int flags);
    void             (*close)(struct vfs_file *f);
    long             (*read)(struct vfs_file *f, void *buf, size_t n);
    long             (*write)(struct vfs_file *f, const void *buf, size_t n);
    long             (*lseek)(struct vfs_file *f, long off, int whence);
    int              (*mkdir)(const char *path);
    int              (*unlink)(const char *path);
    int              (*stat)(const char *path, struct vfs_stat *out);
    bool             (*readdir)(const char *path, size_t *iter, char *name);
};

#endif
