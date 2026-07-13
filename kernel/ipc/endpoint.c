/* kernel/ipc/endpoint.c -- Layer B: rendezvous (see endpoint.h).
 *
 * Concurrency: accept_q/accept_wait are serialized under g_sched_lock, same
 * discipline as channel.c's queues -- so a wake between "check accept_q" and
 * "block" is never lost. */

#include "ipc/endpoint.h"
#include "ipc/handle.h"
#include "process/process.h"
#include "fs/vfs.h"
#include "include/kmalloc.h"
#include "include/kstring.h"
#include "include/errno.h"

static inline uint64_t save_if(void) {
    uint64_t f; __asm__ volatile ("pushfq; pop %0" : "=r"(f) :: "memory"); return f;
}
static inline void restore_if(uint64_t f) {
    if (f & (1ULL << 9)) __asm__ volatile ("sti" ::: "memory");
}

static int32_t g_endpoint_live;
static spinlock_t g_endpoint_glock;
uint32_t endpoint_live_count(void) { return (uint32_t)g_endpoint_live; }

/* Split an absolute path into a resolved PARENT vnode + leaf component.
 * Small local twin of fd.c's static fd_split_parent -- same shape, kept
 * separate rather than exported across an unrelated module boundary. */
static int split_parent(const char *path, struct vnode *parent_out,
                        const char **leaf_out, size_t *leaf_len_out) {
    if (!path || !parent_out || !leaf_out || !leaf_len_out) return -EMBK_EINVAL;

    const char *last_slash = NULL;
    for (const char *s = path; *s != '\0'; s++) if (*s == '/') last_slash = s;
    if (!last_slash) return -EMBK_EINVAL;

    const char *leaf = last_slash + 1;
    size_t leaf_len = 0;
    while (leaf[leaf_len] != '\0') leaf_len++;
    if (leaf_len == 0 || leaf_len > 255) return -EMBK_EINVAL;

    char parent_path[256];
    size_t parent_len = (size_t)(last_slash - path);
    if (parent_len == 0) {
        parent_path[0] = '/'; parent_path[1] = '\0';
    } else {
        if (parent_len >= sizeof(parent_path)) return -EMBK_ENAMETOOLONG;
        memcpy(parent_path, path, parent_len);
        parent_path[parent_len] = '\0';
    }

    int rc = vfs_resolve(parent_path, parent_out);
    if (rc) return rc;

    *leaf_out = leaf;
    *leaf_len_out = leaf_len;
    return EMBK_OK;
}

int endpoint_listen(struct process *owner, const char *path) {
    struct vnode parent;
    const char *leaf; size_t leaf_len;
    int rc = split_parent(path, &parent, &leaf, &leaf_len);
    if (rc) return rc;
    if (!parent.mnt->ops->create_endpoint) return -EMBK_ENOSYS;   /* wrong fs (not epfs) */

    size_t plen = strlen(path);
    if (plen >= sizeof(((struct listen_endpoint *)0)->path)) return -EMBK_ENAMETOOLONG;

    struct listen_endpoint *ep = (struct listen_endpoint *)kmalloc(sizeof(*ep));
    if (!ep) return -EMBK_ENOMEM;
    memset(ep, 0, sizeof(*ep));
    ep->owner = owner;
    ep->owner_pid = owner->pid;
    memcpy(ep->path, path, plen);
    ep->path[plen] = '\0';
    spinlock_init(&ep->lock);

    struct vnode out;
    rc = parent.mnt->ops->create_endpoint(&parent, leaf, leaf_len, ep, &out);
    if (rc) { kfree(ep); return rc; }   /* -EMBK_EEXIST is the common case */

    int handle = obj_handle_alloc(owner, HANDLE_KIND_ENDPOINT, ep);
    if (handle < 0) {
        parent.mnt->ops->unlink(&parent, leaf, leaf_len);
        kfree(ep);
        return handle;
    }
    ep->refcount = 1;
    spin_lock(&g_endpoint_glock);
    g_endpoint_live++;
    spin_unlock(&g_endpoint_glock);
    return handle;
}

int endpoint_accept(struct process *caller, int listen_handle) {
    struct listen_endpoint *ep =
        (struct listen_endpoint *)obj_handle_resolve(caller, listen_handle, HANDLE_KIND_ENDPOINT);
    if (!ep) return -EMBK_EINVAL;

    uint64_t fl = save_if();
    struct channel_end *end = 0;
    for (;;) {
        sched_lock();
        if (ep->accept_count > 0) {
            end = ep->accept_q[ep->accept_head];
            ep->accept_head = (ep->accept_head + 1) % ENDPOINT_ACCEPT_MAX;
            ep->accept_count--;
            sched_unlock();
            break;
        }
        if (ep->unlinked) {   /* shouldn't normally happen: accept() is the owner's own call */
            sched_unlock();
            restore_if(fl);
            return -EMBK_EINVAL;
        }
        sched_block_current_locked(&ep->accept_wait);   /* releases lock on resume */
        restore_if(fl);
    }
    restore_if(fl);

    /* The in-transit ref connect() gave this end becomes the acceptor's
     * handle ref -- no net refcount change (same shape as A6). */
    int ch = obj_handle_alloc(caller, HANDLE_KIND_CHANNEL, end);
    if (ch < 0) {
        /* Table full: this connection can't be delivered. Release the end
         * (drops the in-transit ref, tells the client EPIPE) rather than
         * leak it. */
        channel_release_raw_end(end);
        return ch;
    }
    return ch;
}

int endpoint_connect(struct process *caller, const char *path) {
    struct vnode vn;
    int rc = vfs_resolve(path, &vn);
    if (rc) return rc;
    if (vn.type != VFS_DT_ENDPOINT) return -EMBK_EINVAL;
    if (!vn.mnt->ops->get_endpoint) return -EMBK_ENOSYS;

    struct listen_endpoint *ep = (struct listen_endpoint *)vn.mnt->ops->get_endpoint(&vn);
    if (!ep) return -EMBK_ENOENT;

    /* B4: re-check the owner is still the SAME live process that registered
     * this endpoint (a dead owner's slot could have been recycled by an
     * unrelated new process by now -- same "parent_pid" liveness pattern
     * struct process::parent/parent_pid already uses elsewhere). */
    uint64_t fl0 = save_if();
    sched_lock();
    bool alive = !ep->unlinked && (ep->owner->pid == ep->owner_pid);
    sched_unlock();
    restore_if(fl0);
    if (!alive) return -EMBK_ECONNREFUSED;

    struct channel *chan = channel_create_raw();
    if (!chan) return -EMBK_ENOMEM;

    /* end[0] -> a real handle in the connecting client now. */
    channel_ref_get(chan);
    int ch = obj_handle_alloc(caller, HANDLE_KIND_CHANNEL, &chan->ends[0]);
    if (ch < 0) {
        /* Nothing else references chan yet; release directly (mirrors
         * channel_release_raw_end's bookkeeping without needing a handle). */
        channel_release_raw_end(&chan->ends[0]);
        return ch;
    }

    /* end[1] -> queued on the endpoint's accept_q as an in-transit ref,
     * exactly like a channel message's ancillary handle (A6): released by
     * accept() (becomes a real ref) or by endpoint teardown (dropped). */
    uint64_t fl = save_if();
    sched_lock();
    if (ep->unlinked || ep->owner->pid != ep->owner_pid) {
        /* Raced with the server dying between the liveness check above and
         * here. Undo: close the client's just-installed handle. */
        sched_unlock();
        restore_if(fl);
        obj_handle_free(caller, ch);
        return -EMBK_ECONNREFUSED;
    }
    if (ep->accept_count >= ENDPOINT_ACCEPT_MAX) {
        sched_unlock();
        restore_if(fl);
        obj_handle_free(caller, ch);
        return -EMBK_EAGAIN;   /* server's backlog is full */
    }
    /* We already hold g_sched_lock here -- bump refcount directly. Calling
     * channel_ref_get() would re-enter sched_lock() (non-reentrant) and
     * deadlock; this path (connect's success case) is the only caller that
     * reaches this line with the lock already held. */
    chan->refcount++;        /* the in-transit ref end[1] now carries */
    uint32_t idx = (ep->accept_head + ep->accept_count) % ENDPOINT_ACCEPT_MAX;
    ep->accept_q[idx] = &chan->ends[1];
    ep->accept_count++;
    wait_queue_wake_one(&ep->accept_wait);
    sched_unlock();
    restore_if(fl);

    return ch;
}

/* `locked`: true iff the caller already holds g_sched_lock (process_reap_
 * slot's R2 path) -- see channel_release_raw_end/_locked's comment
 * (channel.h) for why this split exists. When true, the accept_q drain
 * happens directly (no sched_lock/unlock) and the drained ends are released
 * via the _locked channel-release variant too. */
static void endpoint_release_for_handle_impl(struct process *owner, int handle, bool locked) {
    struct listen_endpoint *ep =
        (struct listen_endpoint *)owner->obj_handles[handle].obj;
    if (!ep) return;

    /* B4: unlink the epfs node FIRST (under the endpoint's own lock) so a
     * concurrent connect() either sees ENOENT via vfs_resolve, or -- if it
     * already resolved the vnode and is mid-liveness-check -- observes
     * `unlinked` and refuses. ep->lock is the endpoint's OWN lock (separate
     * from g_sched_lock), so this is always safe regardless of `locked`. */
    spin_lock(&ep->lock);
    ep->unlinked = true;
    spin_unlock(&ep->lock);

    struct vnode parent;
    const char *leaf; size_t leaf_len;
    if (split_parent(ep->path, &parent, &leaf, &leaf_len) == EMBK_OK &&
        parent.mnt->ops->unlink) {
        parent.mnt->ops->unlink(&parent, leaf, leaf_len);
    }

    struct channel_end *drain[ENDPOINT_ACCEPT_MAX];
    uint32_t n = 0;
    uint64_t fl = 0;
    if (!locked) { fl = save_if(); sched_lock(); }
    while (ep->accept_count > 0) {
        drain[n++] = ep->accept_q[ep->accept_head];
        ep->accept_head = (ep->accept_head + 1) % ENDPOINT_ACCEPT_MAX;
        ep->accept_count--;
    }
    wait_queue_wake_all(&ep->accept_wait);
    if (!locked) { sched_unlock(); restore_if(fl); }

    for (uint32_t i = 0; i < n; i++) {
        /* drops the in-transit ref (A6) */
        if (locked) channel_release_raw_end_locked(drain[i]);
        else        channel_release_raw_end(drain[i]);
    }

    spin_lock(&g_endpoint_glock);
    g_endpoint_live--;
    spin_unlock(&g_endpoint_glock);
    kfree(ep);
}

void endpoint_release_for_handle(struct process *owner, int handle) {
    endpoint_release_for_handle_impl(owner, handle, false);
}
void endpoint_release_for_handle_locked(struct process *owner, int handle) {
    endpoint_release_for_handle_impl(owner, handle, true);
}
