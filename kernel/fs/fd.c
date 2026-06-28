#include "fd.h"
#include "vfs.h"
#include "../include/errno.h"
#include "../include/kprintf.h"
#include "../include/kstring.h"
#include <stdint.h>



/* Fds 0/1/2 are reserved for stdin, stdout, stderr, so returned fds start at
 * FD_BASE and map to slot (fd - FD_BASE). */

#define FD_BASE 3
#define FD_MAX_OPEN 64


struct fd_entry {
    bool used;
    struct vnode vn;
    uint64_t pos;
    int flags;
};

static struct fd_entry g_fds[FD_MAX_OPEN];

void vfs_fd_init(void)
{
    for (int i = 0; i < FD_MAX_OPEN; i++) {
        g_fds[i].used = false;
        g_fds[i].pos = 0;
        g_fds[i].flags = 0;
    }
}

/* Map an fd to its live table entry. */
static struct fd_entry *fd_lookup(int fd)
{
    if (fd < FD_BASE || fd >= FD_BASE + FD_MAX_OPEN)
        return NULL;

    struct fd_entry *e = &g_fds[fd - FD_BASE];
    if (!e->used)
        return NULL;

    return e;
}

/* Split an absolute path into parent vnode + leaf component. */
static int fd_split_parent(const char *path, struct vnode *parent_out,
                           const char **leaf_out, size_t *leaf_len_out)
{
    if (!path || !parent_out || !leaf_out || !leaf_len_out)
        return -EMBK_EINVAL;

    const char *last_slash = NULL;
    for (const char *s = path; *s != '\0'; s++) {
        if (*s == '/')
            last_slash = s;
    }

    if (!last_slash)
        return -EMBK_EINVAL;

    const char *leaf = last_slash + 1;
    size_t leaf_len = 0;
    while (leaf[leaf_len] != '\0') leaf_len++;

    if (leaf_len == 0 || leaf_len > 255)
        return -EMBK_EINVAL;

    char parent_path[256];
    size_t parent_len = (size_t)(last_slash - path);
    if (parent_len == 0) {
        parent_path[0] = '/';
        parent_path[1] = '\0';
    } else {
        if (parent_len >= sizeof(parent_path))
            return -EMBK_ENAMETOOLONG;

        for (size_t i = 0; i < parent_len; i++) {
            parent_path[i] = path[i];
        }
        parent_path[parent_len] = '\0';
    }

    int err = vfs_resolve(parent_path, parent_out);
    if (err)
        return err;

    *leaf_out = leaf;
    *leaf_len_out = leaf_len;
    return EMBK_OK;
}

static int fd_unlink_path(const char *path)
{
    struct vnode parent;
    const char *leaf = NULL;
    size_t leaf_len = 0;

    int rc = fd_split_parent(path, &parent, &leaf, &leaf_len);
    if (rc != EMBK_OK)
        return rc;

    if (!parent.mnt || !parent.mnt->ops || !parent.mnt->ops->unlink)
        return -EMBK_ENOSYS;

    return parent.mnt->ops->unlink(&parent, leaf, leaf_len);
}

static bool fd_readable(int flags)
{
    int acc = flags & O_ACCMODE;
    return (acc == O_RDONLY || acc == O_RDWR);
}

static bool fd_writable(int flags)
{
    int acc = flags & O_ACCMODE;
    return (acc == O_WRONLY || acc == O_RDWR);
}

static int fd_seek_compute(uint64_t base, int64_t delta, uint64_t *out)
{
    if (!out)
        return -EMBK_EINVAL;

    if (delta >= 0) {
        uint64_t d = (uint64_t)delta;
        if (d > UINT64_MAX - base)
            return -EMBK_ERANGE;
        *out = base + d;
        return EMBK_OK;
    }

    uint64_t d = (uint64_t)(-(delta + 1)) + 1;
    if (d > base)
        return -EMBK_EINVAL;
    *out = base - d;
    return EMBK_OK;
}


int vfs_open(const char *path, int flags, uint32_t mode)
{
    int acc = flags & O_ACCMODE;
    if (!path)
        return -EMBK_EINVAL;
    if (acc != O_RDONLY && acc != O_WRONLY && acc != O_RDWR)
        return -EMBK_EINVAL;
    if (flags & ~(O_ACCMODE | O_CREAT | O_EXCL | O_TRUNC | O_APPEND))
        return -EMBK_EINVAL;

    struct vnode vn;
    int err = vfs_resolve(path, &vn);
    if (err == EMBK_OK) {
        if ((flags & O_EXCL) && (flags & O_CREAT))
            return -EMBK_EEXIST;
    } else if (err == -EMBK_ENOENT && (flags & O_CREAT)) {
        struct vnode parent;
        const char *leaf;
        size_t leaf_len;
        err = fd_split_parent(path, &parent, &leaf, &leaf_len);
        if (err)
            return err;
        if (!parent.mnt || !parent.mnt->ops || !parent.mnt->ops->create)
            return -EMBK_ENOSYS;

        err = parent.mnt->ops->create(&parent, leaf, leaf_len, mode, &vn);
        if (err)
            return err;
    } else {
        return err;
    }

    int fd = -1;
    for (int i = 0; i < FD_MAX_OPEN; i++) {
        if (!g_fds[i].used) {
            fd = i + FD_BASE;
            break;
        }
    }
    if (fd < 0)
        return -EMBK_EMFILE;

    if (vn.mnt && vn.mnt->ops && vn.mnt->ops->obj_get) {
        err = vn.mnt->ops->obj_get(vn.mnt, vn.ino);
        if (err)
            return err;
    }

    g_fds[fd - FD_BASE].used = true;
    g_fds[fd - FD_BASE].vn = vn;
    g_fds[fd - FD_BASE].pos = 0;
    g_fds[fd - FD_BASE].flags = flags;

    if ((flags & O_APPEND) && vn.mnt && vn.mnt->ops && vn.mnt->ops->stat) {
        struct vfs_stat st;
        err = vn.mnt->ops->stat(&vn, &st);
        if (err == EMBK_OK)
            g_fds[fd - FD_BASE].pos = st.size;
    }

    return fd;
}


int vfs_fd_read(int fd, void *buf, size_t len, size_t *out_read) {
    struct fd_entry *e = fd_lookup(fd);
    if (!e)
        return -EMBK_EBADF;

    if (!buf || !out_read)
        return -EMBK_EINVAL;

    if (!fd_readable(e->flags))
        return -EMBK_EBADF;

    if (!e->vn.mnt || !e->vn.mnt->ops || !e->vn.mnt->ops->read)
        return -EMBK_ENOSYS;

    size_t bytes_read = 0;
    int err = e->vn.mnt->ops->read(&e->vn, e->pos, buf, len, &bytes_read);
    if (err)
        return err;

    e->pos += bytes_read;
    *out_read = bytes_read;
    return EMBK_OK;
}


int vfs_fd_write(int fd, const void *buf, size_t len, size_t *out_written) {
    struct fd_entry *e = fd_lookup(fd);
    if (!e)
        return -EMBK_EBADF;

    if ((!buf && len) || !out_written)
        return -EMBK_EINVAL;

    if (!fd_writable(e->flags))
        return -EMBK_EBADF;

    if (!e->vn.mnt || !e->vn.mnt->ops || !e->vn.mnt->ops->write)
        return -EMBK_ENOSYS;

    if ((e->flags & O_APPEND) && e->vn.mnt->ops->stat) {
        struct vfs_stat st;
        int s = e->vn.mnt->ops->stat(&e->vn, &st);
        if (s)
            return s;
        e->pos = st.size;
    }

    size_t bytes_written = 0;
    int err = e->vn.mnt->ops->write(&e->vn, e->pos, buf, len, &bytes_written);
    if (err)
        return err;

    e->pos += bytes_written;
    *out_written = bytes_written;
    return EMBK_OK;
}


int vfs_fd_seek(int fd, int64_t delta, int whence, uint64_t *out_offset)
{
    struct fd_entry *e = fd_lookup(fd);
    if (!e)
        return -EMBK_EBADF;

    if (!out_offset)
        return -EMBK_EINVAL;

    uint64_t new_pos = 0;
    switch (whence) {
        case 0:
            if (delta < 0)
                return -EMBK_EINVAL;
            new_pos = (uint64_t)delta;
            break;
        case 1: {
            int rc = fd_seek_compute(e->pos, delta, &new_pos);
            if (rc)
                return rc;
            break;
        }
        case 2: {
            if (!e->vn.mnt || !e->vn.mnt->ops || !e->vn.mnt->ops->stat)
                return -EMBK_ENOSYS;

            struct vfs_stat st;
            int err = e->vn.mnt->ops->stat(&e->vn, &st);
            if (err)
                return err;

            int rc = fd_seek_compute(st.size, delta, &new_pos);
            if (rc)
                return rc;
            break;
        }
        default:
            return -EMBK_EINVAL;
    }

    e->pos = new_pos;
    *out_offset = new_pos;
    return EMBK_OK;
}

int vfs_close(int fd)
{
    struct fd_entry *e = fd_lookup(fd);
    if (!e)
        return -EMBK_EBADF;

    int rc = EMBK_OK;
    if (e->vn.mnt && e->vn.mnt->ops && e->vn.mnt->ops->obj_put)
        rc = e->vn.mnt->ops->obj_put(e->vn.mnt, e->vn.ino);

    e->used = false;
    e->pos = 0;
    e->flags = 0;
    e->vn.mnt = NULL;
    e->vn.ino = 0;
    e->vn.type = VFS_DT_UNKNOWN;
    return rc;
}

int vfs_fd_fstat(int fd, struct vfs_stat *out)
{
    struct fd_entry *e = fd_lookup(fd);
    if (!e)
        return -EMBK_EBADF;
    if (!out)
        return -EMBK_EINVAL;
    if (!e->vn.mnt || !e->vn.mnt->ops || !e->vn.mnt->ops->stat)
        return -EMBK_ENOSYS;

    return e->vn.mnt->ops->stat(&e->vn, out);
}

int vfs_fd_run_selftests(void)
{
    const char *path = "/fd_selftest.tmp";
    const char *missing = "/fd_selftest_does_not_exist";
    const char payload[] = "fd-selftest";
    char buf[sizeof(payload)] = {0};
    struct vfs_stat st;
    uint64_t off = 0;
    size_t n = 0;

    kprintf("FD: selftest: begin\n");

    int rc = vfs_open(missing, O_RDONLY, 0);
    if (rc != -EMBK_ENOENT) {
        kprintf("FD: selftest fail: open missing -> %s\n", embk_strerror(rc));
        return rc < 0 ? rc : -EMBK_EINVAL;
    }

    int fd = vfs_open(path, O_CREAT | O_RDWR, 0644);
    if (fd < 0) {
        kprintf("FD: selftest fail: create/open -> %s\n", embk_strerror(fd));
        return fd;
    }

    rc = vfs_fd_write(fd, payload, sizeof(payload) - 1, &n);
    if (rc != EMBK_OK || n != sizeof(payload) - 1) {
        kprintf("FD: selftest fail: write rc=%s n=%u\n", embk_strerror(rc), (unsigned)n);
        vfs_close(fd);
        return (rc != EMBK_OK) ? rc : -EMBK_EIO;
    }

    rc = vfs_fd_seek(fd, 0, 0, &off);
    if (rc != EMBK_OK || off != 0) {
        kprintf("FD: selftest fail: seek set rc=%s off=%lu\n", embk_strerror(rc), off);
        vfs_close(fd);
        return (rc != EMBK_OK) ? rc : -EMBK_EIO;
    }

    rc = vfs_fd_read(fd, buf, sizeof(payload) - 1, &n);
    if (rc != EMBK_OK || n != sizeof(payload) - 1 || memcmp(buf, payload, sizeof(payload) - 1) != 0) {
        kprintf("FD: selftest fail: read/compare rc=%s n=%u\n", embk_strerror(rc), (unsigned)n);
        vfs_close(fd);
        return (rc != EMBK_OK) ? rc : -EMBK_EIO;
    }

    rc = vfs_fd_fstat(fd, &st);
    if (rc != EMBK_OK || st.size < (sizeof(payload) - 1)) {
        kprintf("FD: selftest fail: fstat rc=%s size=%lu\n", embk_strerror(rc), st.size);
        vfs_close(fd);
        return (rc != EMBK_OK) ? rc : -EMBK_EIO;
    }

    rc = vfs_fd_seek(fd, 0, 2, &off);
    if (rc != EMBK_OK || off != st.size) {
        kprintf("FD: selftest fail: seek end rc=%s off=%lu size=%lu\n", embk_strerror(rc), off, st.size);
        vfs_close(fd);
        return (rc != EMBK_OK) ? rc : -EMBK_EIO;
    }

    rc = vfs_close(fd);
    if (rc != EMBK_OK) {
        kprintf("FD: selftest fail: close rc=%s\n", embk_strerror(rc));
        return rc;
    }

    rc = vfs_close(fd);
    if (rc != -EMBK_EBADF) {
        kprintf("FD: selftest fail: close invalid expected EBADF got %s\n", embk_strerror(rc));
        return (rc < 0) ? rc : -EMBK_EINVAL;
    }

    fd = vfs_open(path, O_WRONLY, 0);
    if (fd < 0)
        return fd;
    rc = vfs_fd_read(fd, buf, 1, &n);
    (void)vfs_close(fd);
    if (rc != -EMBK_EBADF) {
        kprintf("FD: selftest fail: read on O_WRONLY expected EBADF got %s\n", embk_strerror(rc));
        return (rc < 0) ? rc : -EMBK_EINVAL;
    }

    fd = vfs_open(path, O_RDONLY, 0);
    if (fd < 0)
        return fd;
    rc = vfs_fd_write(fd, payload, 1, &n);
    (void)vfs_close(fd);
    if (rc != -EMBK_EBADF) {
        kprintf("FD: selftest fail: write on O_RDONLY expected EBADF got %s\n", embk_strerror(rc));
        return (rc < 0) ? rc : -EMBK_EINVAL;
    }

    /* unlink-while-open: the fd is bound to the object, not the name. */
    fd = vfs_open(path, O_RDONLY, 0);
    if (fd < 0)
        return fd;

    rc = fd_unlink_path(path);
    if (rc != EMBK_OK) {
        kprintf("FD: selftest fail: unlink-while-open rc=%s\n", embk_strerror(rc));
        (void)vfs_close(fd);
        return rc;
    }

    int look = vfs_open(path, O_RDONLY, 0);
    if (look != -EMBK_ENOENT) {
        kprintf("FD: selftest fail: name still resolves after unlink -> %s\n", embk_strerror(look));
        if (look >= 0)
            (void)vfs_close(look);
        (void)vfs_close(fd);
        return -EMBK_EINVAL;
    }

    memset(buf, 0, sizeof buf);
    n = 0;
    rc = vfs_fd_seek(fd, 0, 0, &off);
    if (rc == EMBK_OK)
        rc = vfs_fd_read(fd, buf, sizeof(payload) - 1, &n);
    if (rc != EMBK_OK || n != sizeof(payload) - 1 || memcmp(buf, payload, sizeof(payload) - 1) != 0) {
        kprintf("FD: selftest fail: read-after-unlink rc=%s n=%u (object freed early?)\n", embk_strerror(rc), (unsigned)n);
        (void)vfs_close(fd);
        return (rc != EMBK_OK) ? rc : -EMBK_EIO;
    }

    (void)vfs_close(fd);

    rc = fd_unlink_path(path);
    if (rc != EMBK_OK && rc != -EMBK_ENOSYS && rc != -EMBK_ENOENT) {
        kprintf("FD: selftest fail: cleanup unlink rc=%s\n", embk_strerror(rc));
        return rc;
    }

    kprintf("FD: selftest: OK\n");
    return EMBK_OK;
}