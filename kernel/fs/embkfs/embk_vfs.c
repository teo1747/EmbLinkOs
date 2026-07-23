/* kernel/fs/embkfs/embkfs_vfs.c
 *
 * The EMBKFS -> VFS adapter. Turns the neutral vnode/ops calls the VFS makes
 * into EMBKFS's own (vol, dir_oid, name) API. No filesystem logic lives here;
 * it only unwraps vnodes and rewraps results.
 *
 * IT IS ALSO THE FS'S SERIALIZATION POINT -- see THE EMBKFS BIG LOCK below:
 * every op is a thin locked wrapper around an _impl. */

#include "fs/vfs.h"
#include "fs/embkfs/embkfs.h"
#include "include/kprintf.h"
#include "include/errno.h"
#include "process/process.h"   /* sched_lock/sched_block_current_locked/wait_queue */

/* THE EMBKFS BIG LOCK now lives in embkfs.c, with the state it protects (the
 * shared static scratch buffers and the per-volume read cache) -- see the long
 * comment there for why it exists, why it is a sleeping lock, and why it is
 * recursive by owner thread. It moved out of this file so the kernel console's
 * DIRECT embkfs_* calls (`snap`, `test embkfs ...`) could be covered too;
 * while it was a static here, those went around it.
 *
 * These wrappers stay because the bridge still wants an explicit locked region
 * per VFS op: the public API each one calls takes the lock again underneath
 * (recursion makes that free), but several ops call more than one API function
 * and must not let another core interleave BETWEEN them. */
static inline void fs_lock(void)   { embkfs_lock(); }
static inline void fs_unlock(void) { embkfs_unlock(); }

/* The fs_data stashed in a mount for an EMBKFS volume IS the embkfs_volume*
 * the kernel already mounted. This makes the cast explicit and named. */
static inline struct embkfs_volume *vol_of(struct vnode *vn) {
    return (struct embkfs_volume *)vn->mnt->fs_data;
}

/* Map EMBKFS's on-disk dir-entry type to the VFS-neutral type. Explicit on
 * purpose: the two enums can change independently without silently writing a
 * wrong type into a vnode. */
static uint8_t type_from_embk(uint8_t embk_dt) {
    switch (embk_dt) {
        case EMBKFS_DT_DIR: return VFS_DT_DIR;
        case EMBKFS_DT_REG: return VFS_DT_REG;
        case EMBKFS_DT_LNK: return VFS_DT_LNK;
        default:            return VFS_DT_UNKNOWN;
    }
}

/* ---- op impls (all called WITH the big lock held) ---------------------- */

static int lookup_impl(struct vnode *dir, const char *name,
                       size_t name_len, struct vnode *out)
{
    if (!dir || !name || !out || !dir->mnt || !dir->mnt->fs_data)
        return -EMBK_EINVAL;
    if (dir->type != VFS_DT_DIR)
        return -EMBK_ENOTDIR;
    if (name_len == 0 || name_len > 255)
        return -EMBK_EINVAL;

    char comp[256];
    for (size_t i = 0; i < name_len; i++)
        comp[i] = name[i];
    comp[name_len] = '\0';

    struct embkfs_volume *vol = vol_of(dir);
    bool exists = false;
    uint8_t embk_type = EMBKFS_DT_REG;
    uint64_t oid = 0;

    int rc = embkfs_dir_exists_name(vol, dir->ino, comp, &exists, &embk_type, &oid);
    if (rc != EMBK_OK)
        return rc;
    if (!exists)
        return -EMBK_ENOENT;

    out->mnt = dir->mnt;
    out->ino = oid;
    out->type = type_from_embk(embk_type);
    return EMBK_OK;
}

struct embkfs_vfs_readdir_ctx {
    vfs_readdir_cb cb;
    void *ctx;
};

static int embkfs_vfs_readdir_bridge(uint64_t target_oid, uint8_t target_type,
                                     const char *name, uint8_t name_len, void *ctx)
{
    struct embkfs_vfs_readdir_ctx *b = (struct embkfs_vfs_readdir_ctx *)ctx;
    return b->cb(name, name_len, type_from_embk(target_type), target_oid, b->ctx);
}

static int readdir_impl(struct vnode *dir, vfs_readdir_cb cb, void *ctx)
{
    if (!dir || !cb || !dir->mnt || !dir->mnt->fs_data)
        return -EMBK_EINVAL;
    if (dir->type != VFS_DT_DIR)
        return -EMBK_ENOTDIR;

    struct embkfs_vfs_readdir_ctx b = { .cb = cb, .ctx = ctx };
    return embkfs_list_dir(vol_of(dir), dir->ino, embkfs_vfs_readdir_bridge, &b);
}

static int read_impl(struct vnode *vn, uint64_t off, void *buf, size_t len,
                     size_t *out_read)
{
    if (!vn || !buf || !out_read || !vn->mnt || !vn->mnt->fs_data)
        return -EMBK_EINVAL;
    if (vn->type == VFS_DT_DIR)
        return -EMBK_EISDIR;

    uint64_t got = 0;
    int rc = embkfs_read_object_at(vol_of(vn), vn->ino, off, (uint8_t *)buf, len, &got);
    if (rc != EMBK_OK) {
        kprintf("embkfs: READ FAIL oid=%u off=%u len=%u rc=%d\n",
                (unsigned)vn->ino, (unsigned)off, (unsigned)len, (int)rc);
        return rc;
    }

    if (got > (uint64_t)(~(size_t)0))
        return -EMBK_ERANGE;
    *out_read = (size_t)got;
    return EMBK_OK;
}

static int write_impl(struct vnode *vn, uint64_t off, const void *buf, size_t len,
                      size_t *out_written)
{
    if (!vn || (!buf && len) || !out_written || !vn->mnt || !vn->mnt->fs_data)
        return -EMBK_EINVAL;
    if (vn->type == VFS_DT_DIR)
        return -EMBK_EISDIR;

    uint64_t wrote = 0;
    int rc = embkfs_write_object_at(vol_of(vn), vn->ino, off, (const uint8_t *)buf, len, &wrote);
    if (rc != EMBK_OK)
        return rc;

    if (wrote > (uint64_t)(~(size_t)0))
        return -EMBK_ERANGE;
    *out_written = (size_t)wrote;
    return EMBK_OK;
}

static int create_impl(struct vnode *dir, const char *name, size_t name_len,
                       uint32_t mode, struct vnode *out)
{
    (void)mode;
    if (!dir || !name || !out || !dir->mnt || !dir->mnt->fs_data)
        return -EMBK_EINVAL;
    if (dir->type != VFS_DT_DIR)
        return -EMBK_ENOTDIR;
    if (name_len == 0 || name_len > 255)
        return -EMBK_EINVAL;

    char comp[256];
    for (size_t i = 0; i < name_len; i++)
        comp[i] = name[i];
    comp[name_len] = '\0';

    uint64_t oid = 0;
    int rc = embkfs_create_file(vol_of(dir), dir->ino, comp, &oid);
    if (rc != EMBK_OK)
        return rc;

    out->mnt = dir->mnt;
    out->ino = oid;
    out->type = VFS_DT_REG;
    return EMBK_OK;
}

static int mkdir_impl(struct vnode *dir, const char *name, size_t name_len,
                      struct vnode *out)
{
    if (!dir || !name || !out || !dir->mnt || !dir->mnt->fs_data)
        return -EMBK_EINVAL;
    if (dir->type != VFS_DT_DIR)
        return -EMBK_ENOTDIR;
    if (name_len == 0 || name_len > 255)
        return -EMBK_EINVAL;

    char comp[256];
    for (size_t i = 0; i < name_len; i++)
        comp[i] = name[i];
    comp[name_len] = '\0';

    uint64_t oid = 0;
    int rc = embkfs_mkdir_name(vol_of(dir), dir->ino, comp, &oid);
    if (rc != EMBK_OK)
        return rc;

    out->mnt = dir->mnt;
    out->ino = oid;
    out->type = VFS_DT_DIR;
    return EMBK_OK;
}

static int unlink_impl(struct vnode *dir, const char *name, size_t name_len)
{
    if (!dir || !name || !dir->mnt || !dir->mnt->fs_data)
        return -EMBK_EINVAL;
    if (dir->type != VFS_DT_DIR)
        return -EMBK_ENOTDIR;
    if (name_len == 0 || name_len > 255)
        return -EMBK_EINVAL;

    char comp[256];
    for (size_t i = 0; i < name_len; i++)
        comp[i] = name[i];
    comp[name_len] = '\0';

    return embkfs_remove_entry_name(vol_of(dir), dir->ino, comp);
}

/* mtime for `oid`, converted from the inode's ns-since-epoch to the VFS's
 * neutral seconds. Best-effort: a failed inode read leaves 0 ("unknown")
 * rather than failing the whole stat -- size/type are the load-bearing
 * fields, mtime is advisory metadata. */
static uint64_t stat_mtime_seconds(struct embkfs_volume *vol, uint64_t oid)
{
    struct embk_inode_item ino;
    if (embkfs_stat_object(vol, oid, &ino) != EMBK_OK)
        return 0;
    return ino.mtime / 1000000000ULL;
}

static int stat_impl(struct vnode *vn, struct vfs_stat *out)
{
    if (!vn || !out || !vn->mnt || !vn->mnt->fs_data)
        return -EMBK_EINVAL;

    struct embkfs_volume *vol = vol_of(vn);
    out->type = vn->type;
    out->mtime = stat_mtime_seconds(vol, vn->ino);

    if (vn->type == VFS_DT_DIR) {
        out->mode = EMBKFS_S_IFDIR | EMBKFS_PERM_DIR;
        out->nlink = 2;
        uint64_t count = 0;
        int rc = embkfs_dir_entry_count(vol, vn->ino, &count);
        if (rc != EMBK_OK)
            return rc;
        out->size = count;
        return EMBK_OK;
    }

    uint64_t size = 0;
    int rc = embkfs_seek_object(vol, vn->ino, 0, EMBKFS_SEEK_END, 0, &size);
    if (rc != EMBK_OK)
        return rc;

    out->size = size;
    out->nlink = 1;
    if (vn->type == VFS_DT_LNK)
        out->mode = EMBKFS_S_IFLNK | EMBKFS_PERM_LNK;
    else
        out->mode = EMBKFS_S_IFREG | EMBKFS_PERM_FILE;
    return EMBK_OK;
}

/* O_TRUNC's teeth: shrink `vn` to `size` bytes (0 for a plain O_TRUNC open).
 * embkfs_truncate_object is a real COW transaction -- old extents freed,
 * inode size rewritten -- not a cursor trick. */
static int truncate_impl(struct vnode *vn, uint64_t size)
{
    if (!vn || !vn->mnt || !vn->mnt->fs_data)
        return -EMBK_EINVAL;
    if (vn->type == VFS_DT_DIR)
        return -EMBK_EISDIR;
    return embkfs_truncate_object(vol_of(vn), vn->ino, size);
}

/* Remove the EMPTY directory `name` from `dir` -- embkfs_rmdir_name does
 * the emptiness check and refuses otherwise. */
static int rmdir_impl(struct vnode *dir, const char *name, size_t name_len)
{
    if (!dir || !name || !dir->mnt || !dir->mnt->fs_data)
        return -EMBK_EINVAL;
    if (dir->type != VFS_DT_DIR)
        return -EMBK_ENOTDIR;
    if (name_len == 0 || name_len > 255)
        return -EMBK_EINVAL;

    char comp[256];
    for (size_t i = 0; i < name_len; i++)
        comp[i] = name[i];
    comp[name_len] = '\0';

    return embkfs_rmdir_name(vol_of(dir), dir->ino, comp);
}

/* ---- the LOCKED wrappers the ops table points at ------------------------ */

static int embkfs_vfs_lookup(struct vnode *dir, const char *name,
                             size_t name_len, struct vnode *out)
{
    fs_lock();
    int rc = lookup_impl(dir, name, name_len, out);
    fs_unlock();
    return rc;
}

static int embkfs_vfs_readdir(struct vnode *dir, vfs_readdir_cb cb, void *ctx)
{
    fs_lock();
    int rc = readdir_impl(dir, cb, ctx);
    fs_unlock();
    return rc;
}

static int embkfs_vfs_read(struct vnode *vn, uint64_t off, void *buf, size_t len,
                           size_t *out_read)
{
    fs_lock();
    int rc = read_impl(vn, off, buf, len, out_read);
    fs_unlock();
    return rc;
}

static int embkfs_vfs_write(struct vnode *vn, uint64_t off, const void *buf,
                            size_t len, size_t *out_written)
{
    fs_lock();
    int rc = write_impl(vn, off, buf, len, out_written);
    fs_unlock();
    return rc;
}

static int embkfs_vfs_create(struct vnode *dir, const char *name, size_t name_len,
                             uint32_t mode, struct vnode *out)
{
    fs_lock();
    int rc = create_impl(dir, name, name_len, mode, out);
    fs_unlock();
    return rc;
}

static int embkfs_vfs_mkdir(struct vnode *dir, const char *name, size_t name_len,
                            struct vnode *out)
{
    fs_lock();
    int rc = mkdir_impl(dir, name, name_len, out);
    fs_unlock();
    return rc;
}

static int embkfs_vfs_unlink(struct vnode *dir, const char *name, size_t name_len)
{
    fs_lock();
    int rc = unlink_impl(dir, name, name_len);
    fs_unlock();
    return rc;
}

static int embkfs_vfs_stat(struct vnode *vn, struct vfs_stat *out)
{
    fs_lock();
    int rc = stat_impl(vn, out);
    fs_unlock();
    return rc;
}

static int embkfs_vfs_truncate(struct vnode *vn, uint64_t size)
{
    fs_lock();
    int rc = truncate_impl(vn, size);
    fs_unlock();
    return rc;
}

static int embkfs_vfs_rmdir(struct vnode *dir, const char *name, size_t name_len)
{
    fs_lock();
    int rc = rmdir_impl(dir, name, name_len);
    fs_unlock();
    return rc;
}

static int embkfs_vfs_obj_get(struct vfs_mount *mnt, uint64_t ino)
{
    if (!mnt || !mnt->fs_data)
        return -EMBK_EINVAL;
    fs_lock();
    int rc = embkfs_object_get((struct embkfs_volume *)mnt->fs_data, ino);
    fs_unlock();
    return rc;
}

static int embkfs_vfs_obj_put(struct vfs_mount *mnt, uint64_t ino)
{
    if (!mnt || !mnt->fs_data)
        return -EMBK_EINVAL;
    fs_lock();
    int rc = embkfs_object_put((struct embkfs_volume *)mnt->fs_data, ino);
    fs_unlock();
    return rc;
}

static int embkfs_vfs_vget(struct vfs_mount *mnt, uint64_t ino, uint8_t type,
                           struct vnode *out)
{
    if (!mnt || !mnt->fs_data || !out)
        return -EMBK_EINVAL;

    /* No disk I/O: an EMBKFS vnode is just (mnt, oid, type). Pure packaging;
     * the ONE op that deliberately skips the big lock. */
    out->mnt  = mnt;
    out->ino  = ino;
    out->type = type;
    return EMBK_OK;
}

static int embkfs_vfs_rename(struct vnode *old_dir, const char *old_name, size_t old_len,
                             struct vnode *new_dir, const char *new_name, size_t new_len)
{
    if (!old_dir || !new_dir || !old_name || !new_name ||
        !old_dir->mnt || !old_dir->mnt->fs_data)
        return -EMBK_EINVAL;
    if (old_dir->type != VFS_DT_DIR || new_dir->type != VFS_DT_DIR)
        return -EMBK_ENOTDIR;
    if (old_len == 0 || old_len > 255 || new_len == 0 || new_len > 255)
        return -EMBK_EINVAL;

    /* embkfs_rename_name wants NUL-terminated names; leaves arrive as
     * (ptr, len) slices of the caller's path. Same copy mkdir_impl does. */
    char oldz[256], newz[256];
    for (size_t i = 0; i < old_len; i++) oldz[i] = old_name[i];
    oldz[old_len] = '\0';
    for (size_t i = 0; i < new_len; i++) newz[i] = new_name[i];
    newz[new_len] = '\0';

    return embkfs_rename_name(vol_of(old_dir), old_dir->ino, oldz,
                              new_dir->ino, newz);
}

static int embkfs_vfs_chmod(struct vnode *vn, uint32_t mode)
{
    if (!vn || !vn->mnt || !vn->mnt->fs_data)
        return -EMBK_EINVAL;
    fs_lock();
    int rc = embkfs_chmod_object(vol_of(vn), vn->ino, mode);
    fs_unlock();
    return rc;
}

static const struct vfs_ops embkfs_vfs_ops = {
    .lookup = embkfs_vfs_lookup,
    .readdir = embkfs_vfs_readdir,
    .read = embkfs_vfs_read,
    .write = embkfs_vfs_write,
    .create = embkfs_vfs_create,
    .mkdir = embkfs_vfs_mkdir,
    .unlink = embkfs_vfs_unlink,
    .truncate = embkfs_vfs_truncate,
    .rmdir = embkfs_vfs_rmdir,
    .rename = embkfs_vfs_rename,
    .chmod = embkfs_vfs_chmod,
    .stat = embkfs_vfs_stat,
    .vget = embkfs_vfs_vget,
    .obj_get = embkfs_vfs_obj_get,
    .obj_put = embkfs_vfs_obj_put,
};

/* Mount an already-opened EMBKFS volume into the VFS at `path`. */
int embkfs_vfs_register(const char *path, struct embkfs_volume *vol) {
    return vfs_mount(path, &embkfs_vfs_ops, vol, EMBKFS_ROOT_OBJECT_ID);
}
