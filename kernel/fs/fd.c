#include "fs/fd.h"
#include "fs/vfs.h"
#include "include/errno.h"
#include "include/kprintf.h"
#include "include/kstring.h"
#include "process/process.h"
#include "drivers/input/keyboard.h"   /* console fd read: keyboard_getchar_blocking/has_char */
#include "drivers/video/console.h"    /* console fd write: console_putchar */
#include "ipc/pipe.h"                 /* pipe fd backing: pipe_read/write + ref/unref */
#include "kworker/kworker.h"          /* vnode close_locked: defer obj_put off the lock */
#include "include/types.h"
#include <stdint.h>
#include <string.h>



struct process;
/* struct fd_entry + enum fd_backing live in fd.h (struct process embeds the
 * fds[] table, so the layout must be public). struct fd_ops is fd.c-private --
 * fd.h only forward-declares it since fd_entry just holds a pointer. */

/* Boot-time fd table: used only while current_process is NULL (early boot,
 * before any real process exists -- e.g. `test fd`/`test ring3` selftests
 * and enter_user_mode()'s legacy path, all of which run at that point).
 * Once a real process exists, every fd operation uses ITS OWN
 * current_process->fds table instead -- see fd_table() below. This keeps
 * every pre-existing caller working unchanged while giving real processes
 * genuine per-process fd isolation (docs/architecture/process-and-
 * scheduling.md's "no per-process fd table" gap, now closed). */
static struct fd_entry g_boot_fds[FD_MAX_OPEN];

/* The fd table THIS call should operate on: the calling process's own table
 * if one exists, else the shared boot-time table. */
static struct fd_entry *fd_table(void)
{
    return current_thread ? current_process->fds : g_boot_fds;
}

void vfs_fd_init(void)
{
    struct fd_entry *fds = fd_table();
    for (int i = 0; i < FD_MAX_OPEN; i++) {
        fds[i].used = false;
        fds[i].backing = FD_BACKING_NONE;
        fds[i].ops = NULL;
        fds[i].flags = 0;
        fds[i].u.file.pos = 0;
    }
}


/* the per-backing dispatch tables (defined below, near their handlers);
 * forward-declared so the open + stdio-init paths can point new fds at them. */
static const struct fd_ops vnode_fd_ops;
static const struct fd_ops console_fd_ops;
static const struct fd_ops nulldev_fd_ops;

/* Map an fd to its live table entry (in the CALLING process's own table). */
static struct fd_entry *fd_lookup(int fd)
{
    if (fd < FD_BASE || fd >= FD_BASE + FD_MAX_OPEN)
        return NULL;

    struct fd_entry *e = &fd_table()[fd - FD_BASE];
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

/* Public path-level twins, for sys_unlink/sys_mkdir (the shell's rm/mkdir).
 * Same split-parent + per-fs-op dispatch fd_open_into's O_CREAT path uses. */
int vfs_unlink_path(const char *path)
{
    return fd_unlink_path(path);
}

int vfs_mkdir_path(const char *path)
{
    struct vnode parent;
    const char *leaf = NULL;
    size_t leaf_len = 0;

    int rc = fd_split_parent(path, &parent, &leaf, &leaf_len);
    if (rc != EMBK_OK)
        return rc;

    if (!parent.mnt || !parent.mnt->ops || !parent.mnt->ops->mkdir)
        return -EMBK_ENOSYS;

    struct vnode made;
    return parent.mnt->ops->mkdir(&parent, leaf, leaf_len, &made);
}

int vfs_rename_path(const char *old_path, const char *new_path)
{
    struct vnode old_parent, new_parent;
    const char *old_leaf = NULL, *new_leaf = NULL;
    size_t old_len = 0, new_len = 0;

    int rc = fd_split_parent(old_path, &old_parent, &old_leaf, &old_len);
    if (rc != EMBK_OK)
        return rc;
    rc = fd_split_parent(new_path, &new_parent, &new_leaf, &new_len);
    if (rc != EMBK_OK)
        return rc;

    /* One mount today, but say it anyway: a cross-mount rename is a copy, not
     * a rename, and must be refused rather than half-done. */
    if (old_parent.mnt != new_parent.mnt)
        return -EMBK_EXDEV;

    if (!old_parent.mnt || !old_parent.mnt->ops || !old_parent.mnt->ops->rename)
        return -EMBK_ENOSYS;

    return old_parent.mnt->ops->rename(&old_parent, old_leaf, old_len,
                                       &new_parent, new_leaf, new_len);
}

/* ftruncate: fd -> vnode -> the per-fs truncate op that already exists (EMBKFS
 * wires it). Only a VNODE-backed fd can be truncated -- truncating a pipe or
 * the console has no meaning, and EINVAL is what POSIX says for those. */
int vfs_fd_truncate(int fd, uint64_t size)
{
    struct fd_entry *e = fd_lookup(fd);
    if (!e)
        return -EMBK_EBADF;
    if (e->backing != FD_BACKING_VNODE)
        return -EMBK_EINVAL;
    if (!e->u.file.vn.mnt || !e->u.file.vn.mnt->ops || !e->u.file.vn.mnt->ops->truncate)
        return -EMBK_ENOSYS;

    return e->u.file.vn.mnt->ops->truncate(&e->u.file.vn, size);
}

int vfs_chmod_path(const char *path, uint32_t mode)
{
    struct vnode vn;
    int rc = vfs_resolve(path, &vn);
    if (rc != EMBK_OK)
        return rc;
    if (!vn.mnt || !vn.mnt->ops || !vn.mnt->ops->chmod)
        return -EMBK_ENOSYS;
    return vn.mnt->ops->chmod(&vn, mode);
}

int vfs_rmdir_path(const char *path)
{
    struct vnode parent;
    const char *leaf = NULL;
    size_t leaf_len = 0;

    int rc = fd_split_parent(path, &parent, &leaf, &leaf_len);
    if (rc != EMBK_OK)
        return rc;

    if (!parent.mnt || !parent.mnt->ops || !parent.mnt->ops->rmdir)
        return -EMBK_ENOSYS;

    return parent.mnt->ops->rmdir(&parent, leaf, leaf_len);
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

/* Populate a new process's stdio (fds 0/1/2): inherit each from the spawning
 * parent via the per-backing inherit op, or default to the console when there's
 * no parent / no inheritable entry. Called from process_create() BEFORE file
 * actions, so a spawn redirect can override an inherited slot. */
void fds_init_stdio(struct process *proc) {
    struct fd_entry *parent_fds = current_thread ? current_process->fds : NULL;

    for (int i = 0; i < FD_STDIO_MAX; i++) {
        struct fd_entry *dst = &proc->fds[i];
        if (parent_fds && parent_fds[i].used && parent_fds[i].ops
             && parent_fds[i].ops->inherit) {
            if (parent_fds[i].ops->inherit(dst, &parent_fds[i]) == EMBK_OK)
                continue;
            /* Inheritance refused (today: any VNODE --see vnode_fd_inherit).
             * Leaves the slot unset; the child gets EBADF on that fd, which
             * is loud and traceable, rather than silently-shared cursor. */
            memset(dst, 0, sizeof(*dst));
            continue;
        }

        /* No parent (the kernel spawned us -- `home`\init, where
         * current_thread is NULL and the boot context's g_boot_fds has no
         * stdio), or the parent's own slot is empty. Defaults to the console,
         * so the very first userland process has working stdio and the
         * inheritance chain has something to propagate. */
        dst->used = true;
        dst->backing = FD_BACKING_CONSOLE;
        dst->ops = &console_fd_ops;
        dst->flags = (i == 0) ? O_RDONLY : O_WRONLY;  /* stdin vs stdout/stderr */
        memset(&dst->u, 0, sizeof(dst->u));
    }
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

    /* /dev/null: the one special name (see nulldev_fd_ops above). Checked
     * BEFORE VFS resolution -- there is no /dev directory to resolve through,
     * and creating one on disk to host a fake file would be the dishonest
     * version of this. (fd_open_into, the spawn file-action twin below, does
     * not special-case it yet -- add it there when a spawn wants to silence a
     * child's stdio.) */
    if (strcmp(path, "/dev/null") == 0) {
        struct fd_entry *fds = fd_table();
        for (int i = 0; i < FD_MAX_OPEN; i++) {
            if (!fds[i].used) {
                struct fd_entry *e = &fds[i];
                memset(e, 0, sizeof(*e));
                e->used = true;
                e->backing = FD_BACKING_NULLDEV;
                e->ops = &nulldev_fd_ops;
                e->flags = flags;
                return i + FD_BASE;
            }
        }
        return -EMBK_EMFILE;
    }

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

    /* O_TRUNC has teeth now (was reserved): shrink to zero through the
     * per-fs truncate op BEFORE the fd is installed. Requires writable
     * access; a filesystem without the op fails LOUD (-ENOSYS) rather than
     * silently keeping stale bytes past the new writer's data -- the
     * save/cp tail-corruption trap this closes. Truncating a just-created
     * empty file is a cheap no-op, so no created-vs-resolved split. */
    if ((flags & O_TRUNC) && vn.type != VFS_DT_DIR) {
        if (!fd_writable(flags))
            return -EMBK_EINVAL;
        if (!vn.mnt || !vn.mnt->ops || !vn.mnt->ops->truncate)
            return -EMBK_ENOSYS;
        err = vn.mnt->ops->truncate(&vn, 0);
        if (err != EMBK_OK)
            return err;
    }

    struct fd_entry *fds = fd_table();
    int fd = -1;
    for (int i = 0; i < FD_MAX_OPEN; i++) {
        if (!fds[i].used) {
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

    fds[fd - FD_BASE].used = true;
    fds[fd - FD_BASE].backing = FD_BACKING_VNODE;
    fds[fd - FD_BASE].ops = &vnode_fd_ops;
    fds[fd - FD_BASE].u.file.vn = vn;
    fds[fd - FD_BASE].u.file.pos = 0;
    fds[fd - FD_BASE].flags = flags;

    if ((flags & O_APPEND) && vn.mnt && vn.mnt->ops && vn.mnt->ops->stat) {
        struct vfs_stat st;
        err = vn.mnt->ops->stat(&vn, &st);
        if (err == EMBK_OK)
            fds[fd - FD_BASE].u.file.pos = st.size;
    }

    return fd;
}


int vfs_fd_read(int fd, void *buf, size_t len, size_t *out_read) {
    struct fd_entry *e = fd_lookup(fd);
    if (!e)
        return -EMBK_EBADF;

    if (!buf || !out_read)
        return -EMBK_EINVAL;

    if (!e->ops || !e->ops->read)
        return -EMBK_ENOSYS;

    return e->ops->read(e, buf, len, out_read);
}


int vfs_fd_write(int fd, const void *buf, size_t len, size_t *out_written) {
    struct fd_entry *e = fd_lookup(fd);
    if (!e)
        return -EMBK_EBADF;

    if ((!buf && len) || !out_written)
        return -EMBK_EINVAL;

    if (!e->ops || !e->ops->write)
        return -EMBK_ENOSYS;

    return e->ops->write(e, buf, len, out_written);
}


int vfs_fd_seek(int fd, int64_t delta, int whence, uint64_t *out_offset)
{
    struct fd_entry *e = fd_lookup(fd);
    if (!e)
        return -EMBK_EBADF;

    if (!out_offset)
        return -EMBK_EINVAL;

    if (!e->ops || !e->ops->seek)
        return -EMBK_ENOSYS;

    return e->ops->seek(e, delta, whence, out_offset);
}

int vfs_fd_inherit(int fd, struct fd_entry *dst)
{


        struct fd_entry *src = fd_lookup(fd);
    if (!src)
        return -EMBK_EBADF;

    if (!dst)
        return -EMBK_EINVAL;

    if (!src->ops || !src->ops->inherit)
        return -EMBK_ENOSYS;

    return src->ops->inherit(dst, src);
}

int vfs_close(int fd)
{
    struct fd_entry *e = fd_lookup(fd);
    if (!e)
        return -EMBK_EBADF;

    if (!e->ops || !e->ops->close)
        return -EMBK_ENOSYS;

    e->ops->close(e);
    memset(e, 0, sizeof(*e));
    return EMBK_OK;
}

int64_t vfs_fd_avail(int fd) {
    struct fd_entry *e = fd_lookup(fd);
    if (!e)
        return -EMBK_EBADF;
    if (!e->ops || !e->ops->avail)
        return -EMBK_ENOSYS;
    return e->ops->avail(e);
}

int vfs_fd_fstat(int fd, struct vfs_stat *out)
{
    struct fd_entry *e = fd_lookup(fd);
    if (!e)
        return -EMBK_EBADF;
    if (!out)
        return -EMBK_EINVAL;
    if (!e->ops || !e->ops->fstat)
        return -EMBK_ENOSYS;

    return e->ops->fstat(e, out);
}

/* Parallel to vfs_open(), but for an explicit, not-yet-running
 * target process. placing the result at a SPECIFIC fd rather than the 
 * lowest free one. Shares vfs_open()'s underlying logic. only the
 * table and placement logic differ from vfs_open(). 
 *
 * No "close what's already there" step: target->fds[] is guaranteed entirely
 * unused here - process_alloc() never populates it, and spawn() POSIX
 * addopen running against a fork()'d tables that might already have entries. */
 int fd_open_into(struct process *target, int target_fd, const char *path, int flags, uint32_t mode){

    if (!target || !path)
        return -EMBK_EINVAL;
    if (target_fd < FD_BASE || target_fd >= FD_BASE + FD_MAX_OPEN)
        return -EMBK_EINVAL;

    int acc = flags & O_ACCMODE;
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

    /* Same O_TRUNC hook as vfs_open() -- this is the spawn-file-action /
     * redirect path, where a shell's `> file` shape lands. */
    if ((flags & O_TRUNC) && vn.type != VFS_DT_DIR) {
        if (!fd_writable(flags))
            return -EMBK_EINVAL;
        if (!vn.mnt || !vn.mnt->ops || !vn.mnt->ops->truncate)
            return -EMBK_ENOSYS;
        err = vn.mnt->ops->truncate(&vn, 0);
        if (err != EMBK_OK)
            return err;
    }

    struct fd_entry *e = &target->fds[target_fd - FD_BASE];
    if (e->used && e->ops && e->ops->close)
        e->ops->close(e);

    memset(e, 0, sizeof(*e));

    e->used = true;
    e->backing = FD_BACKING_VNODE;
    e->ops = &vnode_fd_ops;
    e->u.file.vn = vn;
    e->u.file.pos = 0;
    e->flags = flags;

    if ((flags & O_APPEND) && vn.mnt && vn.mnt->ops && vn.mnt->ops->stat) {
        struct vfs_stat st;
        err = vn.mnt->ops->stat(&vn, &st);
        if (err == EMBK_OK)
            e->u.file.pos = st.size;
    }

    return target_fd;
 }

static int vnode_fd_read(struct fd_entry *e, void *buf, size_t len, size_t *out_read) {
    if (!fd_readable(e->flags))
        return -EMBK_EBADF;
    if (!e->u.file.vn.mnt || !e->u.file.vn.mnt->ops || !e->u.file.vn.mnt->ops->read)
        return -EMBK_ENOSYS;

    size_t bytes_read = 0;
    int err = e->u.file.vn.mnt->ops->read(&e->u.file.vn, e->u.file.pos, buf, len, &bytes_read);
    if (err)
        return err;

    e->u.file.pos += bytes_read;
    *out_read = bytes_read;
    return EMBK_OK;
}

static int vnode_fd_write(struct fd_entry *e, const void *buf, size_t len, size_t *out_written) {
    if (!fd_writable(e->flags))
        return -EMBK_EBADF;
    if (!e->u.file.vn.mnt || !e->u.file.vn.mnt->ops || !e->u.file.vn.mnt->ops->write)
        return -EMBK_ENOSYS;

    size_t bytes_written = 0;
    int err = e->u.file.vn.mnt->ops->write(&e->u.file.vn, e->u.file.pos, buf, len, &bytes_written);
    if (err)
        return err;

    e->u.file.pos += bytes_written;
    *out_written = bytes_written;
    return EMBK_OK;
}

static int vnode_fd_seek(struct fd_entry *e, int64_t delta, int whence, uint64_t *out_offset) {
    if (!e->u.file.vn.mnt || !e->u.file.vn.mnt->ops || !e->u.file.vn.mnt->ops->stat)
        return -EMBK_ENOSYS;

    struct vfs_stat st;
    int err = e->u.file.vn.mnt->ops->stat(&e->u.file.vn, &st);
    if (err)
        return err;

    uint64_t base;
    switch (whence) {
        case 0: base = 0; break; // SEEK_SET
        case 1: base = e->u.file.pos; break; // SEEK_CUR
        case 2: base = st.size; break; // SEEK_END
        default: return -EMBK_EINVAL;
    }

    uint64_t new_pos;
    err = fd_seek_compute(base, delta, &new_pos);
    if (err)
        return err;

    e->u.file.pos = new_pos;
    *out_offset = new_pos;
    return EMBK_OK;
}


static int vnode_fd_fstat(struct fd_entry *e, struct vfs_stat *out) {
    if (!e->u.file.vn.mnt || !e->u.file.vn.mnt->ops || !e->u.file.vn.mnt->ops->stat)
        return -EMBK_ENOSYS;

    return e->u.file.vn.mnt->ops->stat(&e->u.file.vn, out);
}


static int vnode_fd_inherit(struct fd_entry *dst, const struct fd_entry *src) {
    (void)dst; (void)src;
    /* Deliberately NOT "obj_get + struct copy". That would fix the LIFETIME
     * bug (refcount) while leaving the CURSOR bug: the child would get an
     * independent u.file.pos onto the same object -- neither POSIX (which
     * shares the offset through a shared open-file-description) nor a fresh
     * open. It's an accidental third thing, and it corrupts silently.
     *
     * Nothing can put a vnode in fds 0/1/2 today, so this cannot fire. When
     * something eventually redirects a child's stdout to a FILE, it will
     * fail LOUDLY here rather than quietly sharing a cursor -- and that's
     * the moment to build the real shared open-file-description (the gap
     * already tracked in TODO.md), not before. */
    return -EMBK_ENOSYS;
}


static void vnode_fd_close(struct fd_entry *e) {
    if (e->u.file.vn.mnt && e->u.file.vn.mnt->ops && e->u.file.vn.mnt->ops->obj_put)
        (void)e->u.file.vn.mnt->ops->obj_put(e->u.file.vn.mnt, e->u.file.vn.ino);
}

static void vnode_fd_close_locked(struct fd_entry *e) {
    /* Cannot obj_put here: last-close reads the on-disk link count and may
     * destroy the object (block reclamation + metadata writes) -- real disk
     * I/O, disqualifying under g_sched_lock. Defer to the kworker, which
     * obj_puts from a normal schedulable thread holding nothing. This CLOSES
     * the pre-existing exit-time vnode refcount leak. */
    kworker_defer_obj_put_locked(e->u.file.vn);
}

static const struct fd_ops vnode_fd_ops = {
    .read  = vnode_fd_read,
    .write = vnode_fd_write,
    .seek  = vnode_fd_seek,
    .fstat = vnode_fd_fstat,
    .inherit = vnode_fd_inherit,
    .close = vnode_fd_close,
    .close_locked = vnode_fd_close_locked,
};





static int console_fd_read(struct fd_entry *e, void *buf, size_t len, size_t *out_read) {
    (void)e; 
    if(len == 0) {
        *out_read = 0;
        return EMBK_OK;
    }

    /* Blocks for the FIRST char (a read() on a tty must not return 0 just 
     * because nobody has typed yet -- 0 means EOF), then drains whatever else
     * is already buffered without blocking again. That's line-ish behavior
     * without a line discipline; real canonical mode (echo, backspace
     * handling, line buffering) is a named gap, not built here. */
    char *cbuf = (char *)buf;
    /* Cancellation-aware (docs/INTERRUPTION.md): a console read blocked for input
     * must wake with -ECANCELED when this process is cancelled -- otherwise a
     * program sitting at a prompt could never be ^C'd. */
    int rc = keyboard_getchar_blocking_cancelable(&cbuf[0]);
    if (rc != EMBK_OK) return rc;
    size_t n = 1;
    while (n < len && keyboard_has_char()) {
        cbuf[n++] = keyboard_getchar();         // buffer is non-empty, won't spin
    }
    *out_read = n;
    return EMBK_OK;
}

static int console_fd_write(struct fd_entry *e, const void *buf, size_t len, size_t *out_written) {
    (void)e; 

    const char *cbuf = (const char *)buf;
    for (size_t i = 0; i < len; i++) {
        console_putchar(cbuf[i]);
    }
    *out_written = len;
    return EMBK_OK;
}

static int console_fd_seek(struct fd_entry *e, int64_t delta, int whence, uint64_t *out_offset) {
    (void)e; (void)delta; (void)whence; (void)out_offset;
    return -EMBK_ESPIPE; /* errno.h literally anticipates this : "illegal seek" */
}

static int console_fd_fstat(struct fd_entry *e, struct vfs_stat *out) {
    (void)e; 
    
    /* A character device. Incidentally this is what finally  makes newlib's 
     * _isatty/_fstat stubs HONEST -- they currently claim chardev with no
     * object backing the claim. */

    out->size = 0; /* size is meaningless for a console */
    out->type = VFS_DT_CHAR;
    return EMBK_OK;
}

static int console_fd_inherit(struct fd_entry *dst, const struct fd_entry *src) {
    (void)dst; (void)src;
    
    *dst = *src; /* shallow copy is fine -- console fds are process-global, not per-process */
    return EMBK_OK;
}

static void console_fd_close(struct fd_entry *e) {
    (void)e; 
    /* Console fds are process-global, not per-process. Closing them is
     * meaningless -- the console remains open for everyone. */
}

static int64_t console_fd_avail(struct fd_entry *e) {
    (void)e;
    return keyboard_has_char() ? 1 : 0;
}

static const struct fd_ops console_fd_ops = {
    .read = console_fd_read,
    .write = console_fd_write,
    .seek = console_fd_seek,
    .fstat = console_fd_fstat,
    .inherit = console_fd_inherit,
    .close = console_fd_close,
    .close_locked = NULL,
    .avail = console_fd_avail,
};

/* -------------------------------------------------------------------------
 * /dev/null -- a REAL null device, not a shim. EmbLink has no /dev tree; this
 * one name is special-cased in vfs_open() because a discard-sink/EOF-source is
 * a genuine OS primitive that portable software assumes exists (git's
 * sanitize_stdfds() opens it before doing anything at all; shell redirects
 * want it too). Stateless, like the console backing: reads are instant EOF,
 * writes vanish and report success -- which here is the TRUTH of the object,
 * not a stub pretending. FD_BACKING_NULLDEV, so nothing mistakes it for a file.
 * ------------------------------------------------------------------------- */
static int nulldev_fd_read(struct fd_entry *e, void *buf, size_t len, size_t *out) {
    (void)e; (void)buf; (void)len;
    *out = 0;                      /* always EOF */
    return EMBK_OK;
}
static int nulldev_fd_write(struct fd_entry *e, const void *buf, size_t len, size_t *out) {
    (void)e; (void)buf;
    *out = len;                    /* swallowed, by definition */
    return EMBK_OK;
}
static int nulldev_fd_seek(struct fd_entry *e, int64_t delta, int whence, uint64_t *out_offset) {
    (void)e; (void)delta; (void)whence;
    *out_offset = 0;
    return EMBK_OK;
}
static int nulldev_fd_fstat(struct fd_entry *e, struct vfs_stat *out) {
    (void)e;
    out->size = 0;
    out->type = VFS_DT_CHAR;
    return EMBK_OK;
}
static int nulldev_fd_inherit(struct fd_entry *dst, const struct fd_entry *src) {
    *dst = *src;                   /* stateless singleton: a struct copy IS the inherit */
    return EMBK_OK;
}
static void nulldev_fd_close(struct fd_entry *e) { (void)e; }
static int64_t nulldev_fd_avail(struct fd_entry *e) { (void)e; return 0; }

static const struct fd_ops nulldev_fd_ops = {
    .read = nulldev_fd_read,
    .write = nulldev_fd_write,
    .seek = nulldev_fd_seek,
    .fstat = nulldev_fd_fstat,
    .inherit = nulldev_fd_inherit,
    .close = nulldev_fd_close,
    .close_locked = NULL,
    .avail = nulldev_fd_avail,
};

static int pipe_fd_read(struct fd_entry *e, void *buf, size_t len, size_t *out) {
    if (e->u.pipe.side != 0) return -EMBK_EBADF;    /* direction enforced at the
                                                      * op, per the one-kind design */
    return pipe_read(e->u.pipe.p, buf, len, out);
}
static int pipe_fd_write(struct fd_entry *e, const void *buf, size_t len, size_t *out) {
    if (e->u.pipe.side != 1) return -EMBK_EBADF;
    return pipe_write(e->u.pipe.p, buf, len, out);
}
static int pipe_fd_seek(struct fd_entry *e, int64_t d, int w, uint64_t *out) {
    (void)e;(void)d;(void)w;(void)out; return -EMBK_ESPIPE;
}
static int pipe_fd_fstat(struct fd_entry *e, struct vfs_stat *out) {
    (void)e; out->size = 0; out->type = VFS_DT_FIFO; return EMBK_OK;
    /* FIFO keeps isatty(fd) correctly FALSE when stdio is a pipe -- the
     * mirror-image favor of the console's VFS_DT_CHAR making it true */
}
static int pipe_fd_inherit(struct fd_entry *dst, const struct fd_entry *src) {
    *dst = *src;
    sched_lock();
    pipe_ref_locked(dst->u.pipe.p, dst->u.pipe.side);   /* a copy IS a new
                                                          * reference -- the exact
                                                          * under-ref trap the
                                                          * blanket-memcpy had */
    sched_unlock();
    return EMBK_OK;
}
static void pipe_fd_close(struct fd_entry *e) {
    sched_lock(); pipe_unref_locked(e->u.pipe.p, e->u.pipe.side); sched_unlock();
}
static void pipe_fd_close_locked(struct fd_entry *e) {
    pipe_unref_locked(e->u.pipe.p, e->u.pipe.side);
}

static int64_t pipe_fd_avail(struct fd_entry *e) {
    /* read end: bytes readable; write end: space writable -- both are the
     * same question ("how much moves without blocking?"). */
    return pipe_avail(e->u.pipe.p, e->u.pipe.side);
}

static const struct fd_ops pipe_fd_ops = {
    .read = pipe_fd_read, .write = pipe_fd_write, .seek = pipe_fd_seek,
    .fstat = pipe_fd_fstat, .inherit = pipe_fd_inherit,
    .close = pipe_fd_close, .close_locked = pipe_fd_close_locked,
    .avail = pipe_fd_avail,
};

/* See fd.h. Mirrors fd_open_into's close-then-install redirect idiom: the
 * child's stdio slot was pre-populated by fds_init_stdio (a console, or an
 * inherited entry) -- installing onto it is a REDIRECT, so the old entry is
 * released through its own ops first (console: no-op; an inherited pipe
 * would be properly unref'd rather than leaked). Runs UNLOCKED (spawn's
 * file-actions loop, ordinary syscall context), hence ops->close, and the
 * self-contained sched_lock around the ref bump. */
int fd_install_pipe(struct process *target, int target_fd, struct pipe *p, int side)
{
    if (!target || !p || (side != 0 && side != 1))
        return -EMBK_EINVAL;
    if (target_fd < FD_BASE || target_fd >= FD_BASE + FD_MAX_OPEN)
        return -EMBK_EBADF;

    struct fd_entry *e = &target->fds[target_fd - FD_BASE];
    if (e->used && e->ops && e->ops->close)
        e->ops->close(e);
    memset(e, 0, sizeof(*e));

    e->used = true;
    e->backing = FD_BACKING_PIPE;
    e->ops = &pipe_fd_ops;
    e->flags = (side == 0) ? O_RDONLY : O_WRONLY;
    e->u.pipe.p = p;
    e->u.pipe.side = side;

    sched_lock();
    pipe_ref_locked(p, side);   /* the child is a NEW reference to this end --
                                 * bump before it ever runs, so its close can't
                                 * drop the count below the parent's own hold */
    sched_unlock();
    return EMBK_OK;
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

    /* --- the small-batch trio: O_TRUNC, mtime, rmdir ------------------- */

    /* O_TRUNC: write a LONG payload, reopen with O_TRUNC, write a SHORT
     * one, and assert the file is exactly the short one -- the old tail
     * must be gone (the save/cp corruption this feature closes). */
    const char longp[]  = "AAAAAAAAAAAAAAAA";
    const char shortp[] = "BB";
    fd = vfs_open(path, O_CREAT | O_WRONLY, 0644);
    if (fd < 0) { kprintf("FD: selftest fail: trunc setup open\n"); return fd; }
    rc = vfs_fd_write(fd, longp, sizeof(longp) - 1, &n);
    vfs_close(fd);
    if (rc != EMBK_OK) { kprintf("FD: selftest fail: trunc setup write\n"); return rc; }

    fd = vfs_open(path, O_WRONLY | O_TRUNC, 0);
    if (fd < 0) { kprintf("FD: selftest fail: O_TRUNC open -> %s\n", embk_strerror(fd)); return fd; }
    rc = vfs_fd_write(fd, shortp, sizeof(shortp) - 1, &n);
    vfs_close(fd);
    if (rc != EMBK_OK) { kprintf("FD: selftest fail: post-trunc write\n"); return rc; }

    rc = vfs_stat(path, &st);
    if (rc != EMBK_OK || st.size != sizeof(shortp) - 1) {
        kprintf("FD: selftest fail: O_TRUNC left size=%lu (want %u) -- stale tail!\n",
                (unsigned long)st.size, (unsigned)(sizeof(shortp) - 1));
        return rc != EMBK_OK ? rc : -EMBK_EIO;
    }

    /* mtime: a just-written file must carry a nonzero last-modified. */
    if (st.mtime == 0) {
        kprintf("FD: selftest fail: mtime is 0 on a fresh write\n");
        return -EMBK_EIO;
    }
    (void)vfs_unlink_path(path);

    /* rmdir: mkdir -> rmdir succeeds; rmdir of a NON-EMPTY dir refuses;
     * after cleanup the name resolves to nothing. */
    const char *dpath = "/fd_selftest_dir";
    rc = vfs_mkdir_path(dpath);
    if (rc != EMBK_OK) { kprintf("FD: selftest fail: mkdir -> %s\n", embk_strerror(rc)); return rc; }

    fd = vfs_open("/fd_selftest_dir/inner.tmp", O_CREAT | O_WRONLY, 0644);
    if (fd < 0) { kprintf("FD: selftest fail: create-in-dir\n"); return fd; }
    vfs_close(fd);

    rc = vfs_rmdir_path(dpath);
    if (rc == EMBK_OK) {
        kprintf("FD: selftest fail: rmdir removed a NON-EMPTY dir\n");
        return -EMBK_EIO;
    }
    rc = vfs_unlink_path("/fd_selftest_dir/inner.tmp");
    if (rc != EMBK_OK) { kprintf("FD: selftest fail: unlink inner\n"); return rc; }
    rc = vfs_rmdir_path(dpath);
    if (rc != EMBK_OK) {
        kprintf("FD: selftest fail: rmdir of empty dir -> %s\n", embk_strerror(rc));
        return rc;
    }
    struct vnode gone;
    if (vfs_resolve(dpath, &gone) != -EMBK_ENOENT) {
        kprintf("FD: selftest fail: rmdir'd dir still resolves\n");
        return -EMBK_EIO;
    }

    kprintf("FD: selftest: OK (incl. O_TRUNC + mtime + rmdir)\n");
    return EMBK_OK;
}