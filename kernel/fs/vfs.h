#ifndef _VFS_H
#define _VFS_H

#include "../include/types.h"     /* bool, size_t, NULL */
#include <stdint.h>

/* =====================================================================
 * VFS — the filesystem-neutral layer.
 *
 * Same trick as the block layer, one level up. The block layer made every
 * storage driver look like one `struct embk_block_device` (read/write
 * function pointers + driver_data). The VFS makes every filesystem (EMBKFS,
 * FAT32, ...) look like one `struct vfs_ops`, so the rest of the kernel
 * never calls embkfs_* or fat32_* directly.
 *
 * Path resolution lives HERE, not in the filesystems: the VFS walks a path
 * one component at a time, asking each filesystem only to resolve a single
 * name inside a single directory (->lookup). That single choice is what lets
 * mount points compose and symlinks resolve in one place later.
 * ===================================================================== */

/* ---- Object kinds (vnode->type, vfs_stat.type, readdir type) -------- */
#define VFS_DT_UNKNOWN 0
#define VFS_DT_REG     1   /* regular file */
#define VFS_DT_DIR     2   /* directory    */
#define VFS_DT_LNK     3   /* symlink      */

struct vfs_mount;   /* forward: one mounted filesystem instance      */
struct vfs_ops;     /* forward: the per-filesystem operation table    */

/* ---- vnode: the kernel's handle to one filesystem object -----------
 * Filesystem-neutral. It does NOT cache or own anything yet — a vnode is
 * cheaply synthesized by ->lookup and passed around by pointer. `ino` is the
 * filesystem's OWN identifier for the object, opaque to the VFS:
 *   - EMBKFS: the 64-bit object_id   (root = 1)
 *   - FAT32 : the object's first cluster (root = root_cluster)
 * The VFS never interprets `ino`; it only hands it back to the ops. */
struct vnode {
    struct vfs_mount *mnt;    /* which mounted fs this object lives on  */
    uint64_t          ino;    /* fs-private object id (oid / cluster)   */
    uint8_t           type;   /* VFS_DT_* — learned from the dir entry  */
};

/* ---- stat: the neutral metadata a caller can ask for ----------------
 * EMBKFS fills this straight from its 128-byte inode; FAT32 synthesizes the
 * POSIX-shaped fields (mode from the attr byte, nlink = 1). */
struct vfs_stat {
    uint8_t  type;     /* VFS_DT_*                                      */
    uint32_t mode;     /* POSIX st_mode (type bits + permissions)       */
    uint64_t size;     /* logical size in bytes                         */
    uint64_t nlink;    /* hard-link count (FAT32: always 1)             */
};

/* ---- readdir callback ----------------------------------------------
 * Called once per entry, in the filesystem's natural order. `name` is NOT
 * null-terminated and is valid only for the duration of the call. Return
 * EMBK_OK to continue, or any negative EMBK_* code to stop and propagate. */
typedef int (*vfs_readdir_cb)(const char *name, uint8_t name_len,
                              uint8_t type, uint64_t ino, void *ctx);

/* ---- the per-filesystem operation table (the polymorphism) ----------
 * Each filesystem fills one of these once. Every op takes vnodes, never
 * paths — the VFS has already resolved the path down to (dir, name), or to a
 * single object, before calling. A NULL op means "unsupported"; the VFS
 * returns -EMBK_ENOSYS for it. */
struct vfs_ops {
    /* resolve ONE name inside directory `dir`; fill *out on success */
    int (*lookup)(struct vnode *dir, const char *name, size_t name_len,
                  struct vnode *out);

    /* iterate every entry of directory `dir` */
    int (*readdir)(struct vnode *dir, vfs_readdir_cb cb, void *ctx);

    /* read/write `len` bytes of file `vn` at byte offset `off` */
    int (*read)(struct vnode *vn, uint64_t off, void *buf, size_t len,
                size_t *out_read);
    int (*write)(struct vnode *vn, uint64_t off, const void *buf, size_t len,
                 size_t *out_written);

    /* create a regular file / a directory named `name` inside `dir` */
    int (*create)(struct vnode *dir, const char *name, size_t name_len,
                  uint32_t mode, struct vnode *out);
    int (*mkdir)(struct vnode *dir, const char *name, size_t name_len,
                 struct vnode *out);

    /* remove the name `name` from directory `dir` */
    int (*unlink)(struct vnode *dir, const char *name, size_t name_len);

    /* fill *out with metadata for object `vn` */
    int (*stat)(struct vnode *vn, struct vfs_stat *out);
    int (*vget)(struct vfs_mount *mnt, uint64_t ino, uint8_t type,
                struct vnode *out);
    
    /* Open-handle refcounting hooks. the fd layer calls obj_get when a handle
    * opens, and obj_put when it closes. The filesystem can use this to defer
    * to keep an unlinked but-open object alive until the last handle closes. 
    * A fs without unlink-while-open semantics (e.g. FAT32) leaves both NULL - the fd layer then no-ops. */
    int (*obj_get)(struct vfs_mount *mnt, uint64_t ino);
    int (*obj_put)(struct vfs_mount *mnt, uint64_t ino);
};

/* ---- a mounted filesystem instance ---------------------------------- */
struct vfs_mount {
    const struct vfs_ops *ops;     /* this fs's operation table          */
    void                 *fs_data; /* the embkfs_volume* / fat32_volume* */
    struct vnode          root;    /* the mount's root directory vnode   */
    char                  at[64];  /* mount point, e.g. "/" (one for now)*/
    bool                  used;
};


/* ---- public VFS surface (what the rest of the kernel calls) --------- */
void vfs_init(void);

/* Mount an already-mounted embkfs/fat32 volume at `path`, using `ops`.
 * v1: a single mount, expected to be "/". root_ino is that fs's root id
 * (EMBKFS_ROOT_OBJECT_ID = 1, or FAT32's root_cluster). */
int  vfs_mount(const char *path, const struct vfs_ops *ops, void *fs_data,
               uint64_t root_ino);

/* Resolve an absolute path to a vnode. This is the component-by-component
 * walk — the ONLY path parser in the kernel. */
int  vfs_resolve(const char *path, struct vnode *out);

/* Convenience wrappers: resolve `path`, then call the matching op. */
int  vfs_read(const char *path, uint64_t off, void *buf, size_t len, size_t *out_read);
int  vfs_write(const char *path, uint64_t off, const void *buf, size_t len, size_t *out_written);
int  vfs_readdir(const char *path, vfs_readdir_cb cb, void *ctx);
int  vfs_stat(const char *path, struct vfs_stat *out);



int vfs_ls(const char *path);
int vfs_run_selftests(void);

#endif /* _VFS_H */