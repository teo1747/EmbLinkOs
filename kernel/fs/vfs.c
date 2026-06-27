/* kernel/fs/vfs.c
 *
 * The VFS registry + lifecycle. This file owns the table of mounted
 * filesystems and nothing else yet — path resolution (vfs_resolve) and the
 * convenience wrappers come next.
 *
 * A "mount" here is NOT an act of reading a disk: embkfs_mount()/fat32_mount()
 * already did that and produced a live volume. vfs_mount() only RECORDS that
 * volume in a slot so the rest of the kernel can reach it by path — the same
 * way the block layer records a driver as sda/sdb. The VFS borrows fs_data;
 * it never owns or frees it. */

#include "vfs.h"
#include "../include/errno.h"
#include "../include/kstring.h"   /* strlen, strcmp, memcpy */
#include "../include/kprintf.h"

/* v1 keeps a tiny, fixed table. It MUST be static storage: vfs_mount wires
 * each mount's root vnode to point back at its own slot (root.mnt = &slot),
 * so the slot's address has to stay valid for the life of the kernel. A static
 * array in BSS gives exactly that — fixed addresses, filled in place. */
#define VFS_MAX_MOUNTS 8
static struct vfs_mount g_mounts[VFS_MAX_MOUNTS];

void vfs_init(void)
{
    /* BSS is already zero, but making the reset explicit means vfs_init can be
     * re-run to wipe the table cleanly. used==false marks every slot free. */
    memset(g_mounts, 0, sizeof g_mounts);
}

int vfs_mount(const char *path, const struct vfs_ops *ops, void *fs_data,
              uint64_t root_ino)
{
    if (!path || !ops || !fs_data)
        return -EMBK_EINVAL;

    /* The mount point lives in a fixed char at[64]; it must fit WITH its NUL.
     * Reject (never truncate) anything empty or too long. */
    size_t plen = strlen(path);
    if (plen == 0 || plen >= sizeof g_mounts[0].at)
        return -EMBK_EINVAL;

    /* Refuse a second mount at the same point. Raw string compare — fine for
     * v1; a fuller impl would compare normalized paths. */
    for (int i = 0; i < VFS_MAX_MOUNTS; i++)
        if (g_mounts[i].used && strcmp(g_mounts[i].at, path) == 0)
            return -EMBK_EBUSY;

    /* First free slot. used==false is the "empty" marker. */
    struct vfs_mount *m = NULL;
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!g_mounts[i].used) { m = &g_mounts[i]; break; }
    }
    if (!m)
        return -EMBK_ENOSPC;

    /* Record the borrowed volume + its op table. */
    m->ops     = ops;
    m->fs_data = fs_data;

    /* Bounded copy of the mount point into the fixed buffer. */
    memcpy(m->at, path, plen);
    m->at[plen] = '\0';

    /* THE load-bearing lines. Every op the VFS dispatches begins with
     * vn->mnt->fs_data (see vol_of in the adapter), and every path walk starts
     * from m->root. So the root vnode must point back at THIS slot, carry the
     * filesystem's own root id, and be typed as a directory. Miss this and the
     * first lookup dereferences a bad mnt before EMBKFS is ever called. */
    m->root.mnt  = m;
    m->root.ino  = root_ino;
    m->root.type = VFS_DT_DIR;

    m->used = true;

    kprintf("VFS: mounted fs at \"%s\" (root ino %lu)\n", m->at, root_ino);
    return EMBK_OK;
}