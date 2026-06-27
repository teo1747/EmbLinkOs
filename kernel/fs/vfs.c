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

/* Find the mount whose mount point matches `path`. v1: a simple mount, so this 
 * just returns the one used slot. When real multi-mount lands, this becomes a 
 * longest-prefix match (the deepest mount point that is a prefix of path wins),
 * and it would also hand back the path remainder relative to that mount. */

static struct vfs_mount *vfs_find_mount(const char *path) {

    (void)path;  // unused in v1
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (g_mounts[i].used) {
            return &g_mounts[i];
        }
    }
    return NULL;
}

/* Max path depth breadcrumb stack can hold. A path with more nested
 * components than this is rejected, never overflowed */
#define VFS_MAX_DEPTH 64

/* Resolve an absolute path to a vnode. the ONLY path parser in the kernel:
 * it walk one component at a time, asking each filesystem only -> lookup
 * (one name inside one directory). '.' and '..' are handled specially HERE, in the
 * path layer, so every filesystem get them for free. */
int vfs_resolve(const char *path, struct vnode *out) {
    
    if (!path || !out)
        return -EMBK_EINVAL;
    // v1 only supports absolute paths there is no per-process cwd yet.
    if (path[0] != '/')
        return -EMBK_EINVAL;

    struct vfs_mount *m = vfs_find_mount(path);
    if (!m)
        return -EMBK_ENOENT;                                // no mount at all, so no root to start from

    /* Breadcrumb trail. stack[depth-1] is the directory we're currently inside, 
     * it start s with the mount's root vnode. so '..' at the top is a safe np-op. */
    struct vnode stack[VFS_MAX_DEPTH];
    size_t depth = 0;

    stack[depth++] = m->root;                               // a value coppy - vnode own nothing

    const char *s = path ;  
    while (*s == '/') s++;                                  // skip the leading slash(es)

    while (*s != '\0') {
        /* Carve out one component [comp, comp+len]. */

        const char *comp = s;
        size_t len = 0;
        while (s[len] != '\0' && s[len] != '/') len++;

        if (len == 0 || len > 255) {
            return -EMBK_EINVAL;                             // empty component, e.g. "//" or trailing "/" or too long
        }

        /* Handle special components '.' and '..' without calling the filesystem. */
        if (len == 1 && comp[0] == '.') {
            // '.' stays in the same directory, so just skip it
        } else if (len == 2 && comp[0] == '.' && comp[1] == '.') {
            // '..' goes up one directory, but not above the root
            if (depth > 1) {
                depth--;
            }
        } else {
            /* A real name: ask the current directory to resolve it. The current directory
             * is the last vnode pushed onto the stack. The filesystem fills a new vnode
             * for the child, which we push onto the stack. */
            struct vnode *dir = &stack[depth - 1];

            if (!dir->mnt || !dir->mnt->ops || !dir->mnt->ops->lookup) {
                return -EMBK_ENOSYS;                          // no lookup op for this filesystem
            }

            if (depth >= VFS_MAX_DEPTH) {
                return -EMBK_ENAMETOOLONG;                    // trail too deep
            }       

            struct vnode child;
            int err = dir->mnt->ops->lookup(dir, comp, len, &child);
            if (err)
                return err;                                   // ENOENT, ENOTDIR, EINVAL, or whatever the filesystem returned

            stack[depth++] = child;                           // push the child onto the stack
        }

        /* Advance past this component and skip any trailing slashes. */
        s = comp + len;
        while (*s == '/') s++;                                // skip trailing slashes
    }
    // The last vnode on the stack is the resolved target. Copy it to *out.
    *out = stack[depth - 1];
    return EMBK_OK;
}


/* Public wrappers*/

/* The surface the rest of the kernel uses to interact with the VFS. 
 * Each is the same three steps - resolve, check the op, dispatch. A successful
 * vfs_resolve GUARANTEES vn.mnt and vn.mnt->ops are valid (the root carries
 * them and ->lookup copies them forward), so each wrapper only has to check the
 * one op slot it cares about. A NULL op slot means the fs doesn't support that operation. */

 int vfs_read(const char *path, uint64_t off, void *buf, size_t len, size_t *out_read) {
    
    if (!buf || !out_read)
        return -EMBK_EINVAL;

    struct vnode vn;
    int err = vfs_resolve(path, &vn);
    if (err)
        return err;

    if (!vn.mnt || !vn.mnt->ops || !vn.mnt->ops->read)
        return -EMBK_ENOSYS;

    // Reading a directory path is rejected downstream: the read op returns -EMBK_EISDIR
    // (the adapter checks vn->type). we don't pre-check here
    return vn.mnt->ops->read(&vn, off, buf, len, out_read);
}

int vfs_write(const char *path, uint64_t off, const void *buf, size_t len, size_t *out_written) {
    
    if ((!buf && len) || !out_written)
        return -EMBK_EINVAL;
    /* Struct: the file must already exist. A missing path resolves to -EMBK_ENOENT 
     * and we propagate it. Creation is NOT done here - this function has no `mode`
     * and create needs one. Creat-on-open (O_CREAT) is the open()/fd layer's responsibility. 
     * composing create + write. */
    struct vnode vn;
    int err = vfs_resolve(path, &vn);
    if (err)
        return err;

    if (!vn.mnt || !vn.mnt->ops || !vn.mnt->ops->write)
        return -EMBK_ENOSYS;

    // Writing a directory path is rejected downstream: the write op returns -EMBK_EISDIR
    // (the adapter checks vn->type). we don't pre-check here
    return vn.mnt->ops->write(&vn, off, buf, len, out_written);
}


int vfs_readdir(const char *path, vfs_readdir_cb cb, void *ctx){
    
    if (!cb)
        return -EMBK_EINVAL;

    struct vnode vn;
    int err = vfs_resolve(path, &vn);
    if (err)
        return err;

    if (!vn.mnt || !vn.mnt->ops || !vn.mnt->ops->readdir)
        return -EMBK_ENOSYS;

    if (vn.type != VFS_DT_DIR)
        return -EMBK_ENOTDIR;

    // A non-directory path is rejected downstream: the readdir op returns -EMBK_ENOTDIR
    return vn.mnt->ops->readdir(&vn, cb, ctx);
}


int vfs_stat(const char *path, struct vfs_stat *out) {
    
    if (!out)
        return -EMBK_EINVAL;

    struct vnode vn;
    int err = vfs_resolve(path, &vn);
    if (err)
        return err;

    if (!vn.mnt || !vn.mnt->ops || !vn.mnt->ops->stat)
        return -EMBK_ENOSYS;

    return vn.mnt->ops->stat(&vn, out);
}























































/* kernel/fs/vfs.c — boot-time selftest, append at end of file */

/* Capture the first directory entry readdir hands us, so the selftest can
 * resolve a REAL name without hardcoding one. name is not NUL-terminated and
 * only valid during the call, so we copy it out. */
struct vfs_first_ent {
    bool     have;
    char     name[256];
    uint8_t  name_len;
    uint64_t ino;
    uint8_t  type;
};

static int vfs_first_ent_cb(const char *name, uint8_t name_len,
                            uint8_t type, uint64_t ino, void *ctx)
{
    struct vfs_first_ent *f = (struct vfs_first_ent *)ctx;
    if (!f->have) {
        for (uint8_t i = 0; i < name_len; i++) f->name[i] = name[i];
        f->name[name_len] = '\0';
        f->name_len = name_len;
        f->ino = ino;
        f->type = type;
        f->have = true;
    }
    return EMBK_OK;   /* keep iterating; we only keep the first */
}

int vfs_run_selftests(void)
{
    struct vfs_mount *m = vfs_find_mount("/");
    if (!m)
        return -EMBK_ENODEV;          /* nothing mounted to test against */

    kprintf("VFS: selftest: begin\n");
    bool ok = true;
    int rc;
    struct vnode vn;

    /* 1. "/" resolves to the mount root (the empty-walk case). Compared
     *    against the mount's OWN root, so this stays fs-neutral. */
    rc = vfs_resolve("/", &vn);
    if (rc != EMBK_OK || vn.ino != m->root.ino || vn.type != m->root.type) {
        kprintf("VFS:   FAIL '/' rc=%d ino=%lu type=%u\n", rc, vn.ino, vn.type);
        ok = false;
    }

    /* 2. "/." stays on root ('.' is a no-op). */
    rc = vfs_resolve("/.", &vn);
    if (rc != EMBK_OK || vn.ino != m->root.ino) {
        kprintf("VFS:   FAIL '/.' rc=%d ino=%lu\n", rc, vn.ino);
        ok = false;
    }

    /* 3. "/.." cannot rise above root — this is the depth>1 guard you
     *    described. If it underflowed, ino here would be garbage. */
    rc = vfs_resolve("/..", &vn);
    if (rc != EMBK_OK || vn.ino != m->root.ino) {
        kprintf("VFS:   FAIL '/..' rc=%d ino=%lu\n", rc, vn.ino);
        ok = false;
    }

    /* 4. A relative path is rejected (no per-process cwd in v1). */
    rc = vfs_resolve("nope", &vn);
    if (rc != -EMBK_EINVAL) {
        kprintf("VFS:   FAIL relative rc=%d (want -EINVAL)\n", rc);
        ok = false;
    }

    /* 5. A name that can't exist must propagate ENOENT up from ->lookup.
     *    This is the only check that proves the walk really reaches the
     *    filesystem, not just the special-component branches. */
    rc = vfs_resolve("/__vfs_selftest_absent__", &vn);
    if (rc != -EMBK_ENOENT) {
        kprintf("VFS:   FAIL absent rc=%d (want -ENOENT)\n", rc);
        ok = false;
    }

    /* 6. Discover a real root entry via readdir, resolve it, and confirm we
     *    land on the SAME object id. Image-agnostic positive test: exercises
     *    ->readdir, ->lookup and the full walk at once. */
    struct vfs_first_ent f = { .have = false };
    if (m->root.mnt->ops->readdir) {
        rc = m->root.mnt->ops->readdir(&m->root, vfs_first_ent_cb, &f);
        if (rc != EMBK_OK) {
            kprintf("VFS:   FAIL readdir root rc=%d\n", rc);
            ok = false;
        } else if (f.have) {
            char p[258];
            p[0] = '/';
            for (uint8_t i = 0; i < f.name_len; i++) p[1 + i] = f.name[i];
            p[1 + f.name_len] = '\0';

            rc = vfs_resolve(p, &vn);
            if (rc != EMBK_OK || vn.ino != f.ino) {
                kprintf("VFS:   FAIL resolve '%s' rc=%d ino=%lu want=%lu\n",
                        p, rc, vn.ino, f.ino);
                ok = false;
            } else {
                kprintf("VFS:   resolved '%s' -> ino %lu OK\n", p, vn.ino);
            }
        } else {
            kprintf("VFS:   (root empty — skipped live resolve)\n");
        }
    }

    kprintf("VFS: selftest: %s\n", ok ? "PASS" : "FAIL");
    return ok ? EMBK_OK : -EMBK_EINVAL;
}

