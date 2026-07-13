/* kernel/fs/epfs.c -- the RAM-backed endpoint filesystem (see epfs.h).
 *
 * A fixed table of nodes; node index + 1 is its `ino` (index 0 is reserved
 * so ino 0 stays an unambiguous "invalid" sentinel, the same convention
 * EMBKFS's own OBJID_ROOT=1 numbering follows). Root (ino EPFS_ROOT_INO=1)
 * is a permanent directory with no parent; every other used node is either
 * a directory (unused in v1 -- root is the only one ever created) or an
 * VFS_DT_ENDPOINT bound to an opaque struct listen_endpoint* the IPC layer
 * owns. epfs itself never looks inside that pointer -- it's just cargo. */

#include "fs/epfs.h"
#include "include/errno.h"
#include "include/kstring.h"

#define EPFS_MAX_NODES 64
#define EPFS_NAME_MAX  32

enum epfs_node_type { EPFS_FREE = 0, EPFS_DIR, EPFS_ENDPOINT };

struct epfs_node {
    enum epfs_node_type type;
    char                 name[EPFS_NAME_MAX];
    uint64_t             parent_ino;   /* 0 only for root itself */
    void                *endpoint;     /* struct listen_endpoint*, EPFS_ENDPOINT only */
    bool                 used;
};

static struct epfs_node g_nodes[EPFS_MAX_NODES];
static int g_epfs_mount_sentinel;   /* vfs_mount() requires non-NULL fs_data;
                                    * epfs's real state is this file's own
                                    * static g_nodes[], so the pointer's
                                    * VALUE is never dereferenced -- it only
                                    * needs to be non-NULL and constant. */

void epfs_init(void) {
    memset(g_nodes, 0, sizeof(g_nodes));
    /* Root: index 0 -> ino 1. */
    g_nodes[0].used = true;
    g_nodes[0].type = EPFS_DIR;
    g_nodes[0].name[0] = '\0';
    g_nodes[0].parent_ino = 0;
}

static struct epfs_node *node_at_ino(uint64_t ino) {
    if (ino < 1 || ino > EPFS_MAX_NODES) return NULL;
    struct epfs_node *n = &g_nodes[ino - 1];
    return n->used ? n : NULL;
}

static int find_child(uint64_t parent_ino, const char *name, size_t name_len) {
    if (name_len == 0 || name_len >= EPFS_NAME_MAX) return -1;
    for (int i = 0; i < EPFS_MAX_NODES; i++) {
        struct epfs_node *n = &g_nodes[i];
        if (!n->used || n->parent_ino != parent_ino) continue;
        if (strlen(n->name) == name_len && memcmp(n->name, name, name_len) == 0) {
            return i;
        }
    }
    return -1;
}

static uint8_t vfs_type_of(enum epfs_node_type t) {
    return t == EPFS_DIR ? VFS_DT_DIR : VFS_DT_ENDPOINT;
}

/* ---- vfs_ops ------------------------------------------------------------ */

static int epfs_lookup(struct vnode *dir, const char *name, size_t name_len,
                       struct vnode *out) {
    if (!dir || !name || !out) return -EMBK_EINVAL;
    if (dir->type != VFS_DT_DIR) return -EMBK_ENOTDIR;

    int idx = find_child(dir->ino, name, name_len);
    if (idx < 0) return -EMBK_ENOENT;

    out->mnt  = dir->mnt;
    out->ino  = (uint64_t)idx + 1;
    out->type = vfs_type_of(g_nodes[idx].type);
    return EMBK_OK;
}

static int epfs_readdir(struct vnode *dir, vfs_readdir_cb cb, void *ctx) {
    if (!dir || !cb) return -EMBK_EINVAL;
    if (dir->type != VFS_DT_DIR) return -EMBK_ENOTDIR;

    for (int i = 0; i < EPFS_MAX_NODES; i++) {
        struct epfs_node *n = &g_nodes[i];
        if (!n->used || n->parent_ino != dir->ino) continue;
        int rc = cb(n->name, (uint8_t)strlen(n->name), vfs_type_of(n->type),
                   (uint64_t)i + 1, ctx);
        if (rc != EMBK_OK) return rc;
    }
    return EMBK_OK;
}

static int epfs_stat(struct vnode *vn, struct vfs_stat *out) {
    if (!vn || !out) return -EMBK_EINVAL;
    struct epfs_node *n = node_at_ino(vn->ino);
    if (!n) return -EMBK_ENOENT;

    out->type  = vfs_type_of(n->type);
    out->mode  = (n->type == EPFS_DIR) ? 0755 : 0600;
    out->size  = 0;
    out->nlink = 1;
    return EMBK_OK;
}

static int epfs_create_endpoint(struct vnode *dir, const char *name, size_t name_len,
                                void *endpoint_obj, struct vnode *out) {
    if (!dir || !name || !endpoint_obj || !out) return -EMBK_EINVAL;
    if (dir->type != VFS_DT_DIR) return -EMBK_ENOTDIR;
    if (name_len == 0 || name_len >= EPFS_NAME_MAX) return -EMBK_ENAMETOOLONG;

    if (find_child(dir->ino, name, name_len) >= 0) return -EMBK_EEXIST;

    int slot = -1;
    for (int i = 0; i < EPFS_MAX_NODES; i++) {
        if (!g_nodes[i].used) { slot = i; break; }
    }
    if (slot < 0) return -EMBK_ENOSPC;

    struct epfs_node *n = &g_nodes[slot];
    n->used = true;
    n->type = EPFS_ENDPOINT;
    memcpy(n->name, name, name_len);
    n->name[name_len] = '\0';
    n->parent_ino = dir->ino;
    n->endpoint = endpoint_obj;

    out->mnt  = dir->mnt;
    out->ino  = (uint64_t)slot + 1;
    out->type = VFS_DT_ENDPOINT;
    return EMBK_OK;
}

static void *epfs_get_endpoint(struct vnode *vn) {
    if (!vn || vn->type != VFS_DT_ENDPOINT) return NULL;
    struct epfs_node *n = node_at_ino(vn->ino);
    if (!n || n->type != EPFS_ENDPOINT) return NULL;
    return n->endpoint;
}

static int epfs_unlink(struct vnode *dir, const char *name, size_t name_len) {
    if (!dir) return -EMBK_EINVAL;
    if (dir->type != VFS_DT_DIR) return -EMBK_ENOTDIR;

    int idx = find_child(dir->ino, name, name_len);
    if (idx < 0) return -EMBK_ENOENT;
    if (g_nodes[idx].type == EPFS_DIR) return -EMBK_ENOSYS;   /* no subdirs to remove in v1 */

    memset(&g_nodes[idx], 0, sizeof(g_nodes[idx]));
    return EMBK_OK;
}

static const struct vfs_ops epfs_vfs_ops = {
    .lookup          = epfs_lookup,
    .readdir         = epfs_readdir,
    .stat            = epfs_stat,
    .create_endpoint = epfs_create_endpoint,
    .get_endpoint    = epfs_get_endpoint,
    .unlink          = epfs_unlink,
    /* .read/.write/.create/.mkdir/.vget/.obj_get/.obj_put: NULL -- epfs
     * holds no byte-stream files and no subdirectories in v1. */
};

int epfs_vfs_register(const char *path) {
    return vfs_mount(path, &epfs_vfs_ops, &g_epfs_mount_sentinel, EPFS_ROOT_INO);
}

/* ---- direct (non-VFS-op) accessors the IPC endpoint layer needs ---------
 * chan_listen/connect resolve the PARENT directory via vfs_resolve (a
 * generic VFS call) and then need the fs-specific create_endpoint/lookup
 * ops -- both already exposed above through struct vfs_ops, reached via
 * vn.mnt->ops->create_endpoint / ->lookup exactly like every other
 * filesystem op. No separate epfs-private API is needed. */
