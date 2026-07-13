/* kernel/ipc/channel.c -- Layer A: message-oriented IPC channels.
 *
 * Concurrency model: the message queues, the peer_closed flags, and all
 * blocking are serialized under the GLOBAL scheduler lock (via sched_lock/
 * sched_unlock, process.c). This is deliberate -- the kernel's wait-queue
 * machinery is g_sched_lock-protected, so checking "inbox empty/full" and
 * then blocking must happen under that same lock or a wake between the two is
 * lost. Channels are a low-frequency control path (bulk pixels go through
 * shared surfaces, not payloads), so serializing them globally is fine.
 *
 * Handle refcounting across the queue (invariant A6): a surface refcount
 * counts outstanding references = handles + in-transit message refs. COPY
 * bumps the ref at send (held in-transit); the receiver's installed handle
 * takes over that ref at recv (no net change). MOVE consumes the sender's
 * handle (its ref becomes the in-transit ref, no net change). If a message
 * dies undelivered (channel closed while queued), its in-transit refs are
 * dropped -- so a surface can't leak on a dropped handoff. */

#include "ipc/channel.h"
#include "ipc/handle.h"
#include "process/process.h"
#include "gfx/surface.h"
#include "include/kmalloc.h"
#include "include/kstring.h"
#include "include/errno.h"

static uint32_t g_chan_next_id;
static int32_t  g_chan_live;
static spinlock_t g_chan_glock;

uint32_t channel_live_count(void) { return (uint32_t)g_chan_live; }

/* Snapshot/restore the caller's interrupt-enable bit around a block (the
 * resumed thread's IF isn't otherwise restored on this non-iretq path -- same
 * reason process_wait/thread_join do this). */
static inline uint64_t save_if(void) {
    uint64_t f; __asm__ volatile ("pushfq; pop %0" : "=r"(f) :: "memory"); return f;
}
static inline void restore_if(uint64_t f) {
    if (f & (1ULL << 9)) __asm__ volatile ("sti" ::: "memory");
}

/* ---- ancillary-object refcount dispatch (v1: surfaces) ------------------ */
static void obj_ref_get(enum handle_kind k, void *obj) {
    if (k == HANDLE_KIND_SURFACE) surface_ref_get((struct surface *)obj);
    /* CHANNEL/ENDPOINT ancillary passing is not supported in v1. */
}
static void obj_ref_put(enum handle_kind k, void *obj) {
    if (k == HANDLE_KIND_SURFACE) surface_ref_put((struct surface *)obj);
}

/* Free one message and, since it was never delivered, drop the in-transit
 * ref each ancillary handle held (A6). Call OUTSIDE g_sched_lock. */
static void chan_msg_free_undelivered(struct chan_msg *m) {
    for (uint32_t i = 0; i < m->n_handles; i++) {
        obj_ref_put(m->handles[i].kind, m->handles[i].obj);
    }
    if (m->bytes) kfree(m->bytes);
    kfree(m);
}

/* ---- create a connected pair ------------------------------------------- */
int channel_create_pair(struct process *owner, int *out_h0, int *out_h1) {
    struct channel *chan = (struct channel *)kmalloc(sizeof(*chan));
    if (!chan) return -EMBK_ENOMEM;
    memset(chan, 0, sizeof(*chan));
    spinlock_init(&chan->lock);
    chan->ends[0].chan = chan; chan->ends[0].side = 0;
    chan->ends[1].chan = chan; chan->ends[1].side = 1;
    chan->refcount = 2;   /* both ends open */

    spin_lock(&g_chan_glock);
    chan->id = ++g_chan_next_id;
    g_chan_live++;
    spin_unlock(&g_chan_glock);

    int h0 = obj_handle_alloc(owner, HANDLE_KIND_CHANNEL, &chan->ends[0]);
    if (h0 < 0) { spin_lock(&g_chan_glock); g_chan_live--; spin_unlock(&g_chan_glock); kfree(chan); return h0; }
    int h1 = obj_handle_alloc(owner, HANDLE_KIND_CHANNEL, &chan->ends[1]);
    if (h1 < 0) {
        owner->obj_handles[h0].used = false;
        owner->obj_handles[h0].kind = HANDLE_KIND_NONE;
        owner->obj_handles[h0].obj = 0;
        spin_lock(&g_chan_glock); g_chan_live--; spin_unlock(&g_chan_glock);
        kfree(chan);
        return h1;
    }
    *out_h0 = h0; *out_h1 = h1;
    return 0;
}

/* Raw channel, no handles/refcount installed -- see channel.h. */
struct channel *channel_create_raw(void) {
    struct channel *chan = (struct channel *)kmalloc(sizeof(*chan));
    if (!chan) return 0;
    memset(chan, 0, sizeof(*chan));
    spinlock_init(&chan->lock);
    chan->ends[0].chan = chan; chan->ends[0].side = 0;
    chan->ends[1].chan = chan; chan->ends[1].side = 1;
    chan->refcount = 0;   /* caller (endpoint.c) bumps as each end is claimed */

    spin_lock(&g_chan_glock);
    chan->id = ++g_chan_next_id;
    g_chan_live++;
    spin_unlock(&g_chan_glock);
    return chan;
}

void channel_ref_get(struct channel *chan) {
    uint64_t fl = save_if();
    sched_lock();
    chan->refcount++;
    sched_unlock();
    restore_if(fl);
}

/* ---- send -------------------------------------------------------------- */
int64_t channel_send(struct process *caller, int handle,
                     const void *bytes, uint32_t len,
                     const int *hnds, const int *flags, uint32_t n_hnd) {
    struct channel_end *end = (struct channel_end *)obj_handle_resolve(caller, handle, HANDLE_KIND_CHANNEL);
    if (!end) return -EMBK_EINVAL;
    if (len > CHAN_MSG_MAX_BYTES) return -EMBK_EMSGSIZE;
    if (n_hnd > CHAN_MSG_MAX_HANDLES) return -EMBK_EINVAL;

    struct channel *chan = end->chan;
    int side = end->side;
    struct channel_end *peer = &chan->ends[1 - side];

    /* Build the message off to the side (payload copy + handle validation)
     * before touching the lock. Validate ALL handles first so a bad one
     * aborts with nothing consumed. */
    struct chan_msg *msg = (struct chan_msg *)kmalloc(sizeof(*msg));
    if (!msg) return -EMBK_ENOMEM;
    memset(msg, 0, sizeof(*msg));
    msg->len = len;
    if (len) {
        msg->bytes = (uint8_t *)kmalloc(len);
        if (!msg->bytes) { kfree(msg); return -EMBK_ENOMEM; }
        memcpy(msg->bytes, bytes, len);
    }
    msg->n_handles = n_hnd;
    for (uint32_t i = 0; i < n_hnd; i++) {
        int hi = hnds[i];
        if (hi < 0 || hi >= OBJ_HANDLE_MAX || !caller->obj_handles[hi].used) {
            if (msg->bytes) kfree(msg->bytes);
            kfree(msg);
            return -EMBK_EINVAL;   /* bad ancillary handle -- nothing consumed yet */
        }
        msg->handles[i].kind    = caller->obj_handles[hi].kind;
        msg->handles[i].obj     = caller->obj_handles[hi].obj;
        msg->handles[i].is_move = (flags && flags[i] == CHAN_HANDLE_MOVE);
    }

    uint64_t fl = save_if();
    sched_lock();
    /* Peer gone? fail atomically -- nothing consumed. */
    if (chan->peer_closed[side]) {
        sched_unlock();
        if (msg->bytes) kfree(msg->bytes);
        kfree(msg);
        return -EMBK_EPIPE;
    }
    /* Backpressure (A3): block until the peer inbox has room (or peer dies). */
    while (peer->inbox_count >= CHAN_QUEUE_MAX && !chan->peer_closed[side]) {
        sched_block_current_locked(&peer->send_wait);   /* releases lock on resume */
        restore_if(fl);
        sched_lock();
    }
    if (chan->peer_closed[side]) {
        sched_unlock();
        if (msg->bytes) kfree(msg->bytes);
        kfree(msg);
        return -EMBK_EPIPE;
    }

    /* Space confirmed + peer alive: NOW apply the per-handle copy/move ref
     * semantics (still under the lock, so nothing races the sender's table). */
    for (uint32_t i = 0; i < n_hnd; i++) {
        int hi = hnds[i];
        if (msg->handles[i].is_move) {
            /* MOVE: relinquish the sender's handle (its ref travels with the
             * message). Unmap surfaces so exit-cleanup won't free shared pages. */
            if (msg->handles[i].kind == HANDLE_KIND_SURFACE) {
                surface_unmap_from_handle(caller, hi);
            }
            caller->obj_handles[hi].used = false;
            caller->obj_handles[hi].kind = HANDLE_KIND_NONE;
            caller->obj_handles[hi].obj  = 0;
        } else {
            /* COPY: bump the ref, held in-transit; sender keeps its handle. */
            obj_ref_get(msg->handles[i].kind, msg->handles[i].obj);
        }
    }

    /* Enqueue on the peer's inbox; wake one recv-blocked thread there (A1). */
    msg->next = NULL;
    if (peer->inbox_tail) peer->inbox_tail->next = msg;
    else                  peer->inbox_head = msg;
    peer->inbox_tail = msg;
    peer->inbox_count++;
    wait_queue_wake_one(&peer->recv_wait);
    sched_unlock();
    restore_if(fl);
    return 0;
}

/* ---- recv -------------------------------------------------------------- */
int64_t channel_recv(struct process *caller, int handle,
                     void *kbuf, uint32_t buflen,
                     uint32_t *out_len, int *out_hnds, uint32_t *out_nhnd) {
    struct channel_end *end = (struct channel_end *)obj_handle_resolve(caller, handle, HANDLE_KIND_CHANNEL);
    if (!end) return -EMBK_EINVAL;
    struct channel *chan = end->chan;
    int side = end->side;

    uint64_t fl = save_if();
    struct chan_msg *msg = NULL;
    for (;;) {
        sched_lock();
        if (end->inbox_count > 0) {
            msg = end->inbox_head;
            if (msg->len > buflen) {
                /* Message-oriented: don't split. Leave it queued; caller
                 * retries with a bigger buffer. */
                sched_unlock();
                return -EMBK_EMSGSIZE;
            }
            end->inbox_head = msg->next;
            if (!end->inbox_head) end->inbox_tail = NULL;
            end->inbox_count--;
            wait_queue_wake_one(&end->send_wait);   /* space freed for senders (A3) */
            sched_unlock();
            break;
        }
        if (chan->peer_closed[side]) {              /* A4: peer hung up + empty */
            sched_unlock();
            return -EMBK_EPIPE;
        }
        sched_block_current_locked(&end->recv_wait); /* releases lock on resume (A2) */
        restore_if(fl);
    }
    restore_if(fl);

    /* Deliver: copy payload, install ancillary handles into the receiver's
     * table (each takes over the in-transit ref -- no net refcount change). */
    if (msg->len && kbuf) memcpy(kbuf, msg->bytes, msg->len);
    if (out_len) *out_len = msg->len;

    uint32_t n = 0;
    for (uint32_t i = 0; i < msg->n_handles; i++) {
        int nh = obj_handle_alloc(caller, msg->handles[i].kind, msg->handles[i].obj);
        if (nh < 0) {
            /* Receiver table full -- this handle can't be delivered; drop its
             * in-transit ref so it doesn't leak. */
            obj_ref_put(msg->handles[i].kind, msg->handles[i].obj);
            continue;
        }
        if (out_hnds) out_hnds[n] = nh;
        n++;
    }
    if (out_nhnd) *out_nhnd = n;

    if (msg->bytes) kfree(msg->bytes);
    kfree(msg);   /* NOT chan_msg_free_undelivered: refs were handed to the receiver */
    return 0;
}

/* ---- close / teardown --------------------------------------------------
 * _locked variants ASSUME g_sched_lock is already held by the caller and
 * never call sched_lock/sched_unlock themselves -- required because
 * process_reap_slot() (process.c), which obj_handles_release_all() (R2
 * cleanup) runs inside, is ALWAYS invoked with g_sched_lock already held
 * (a structural invariant of this scheduler: every existing call site does
 * this). The self-locking public versions are for ordinary syscall context
 * (e.g. sys_chan_close), where the lock is NOT already held. Mixing them up
 * is a real reentrant-spinlock deadlock, not a theoretical one -- it's
 * exactly what surfaced the first time a selftest let R2 close a channel
 * automatically instead of an explicit sys_chan_close(). */
void channel_release_raw_end_locked(struct channel_end *end) {
    struct channel *chan = end->chan;
    int side = end->side;

    chan->peer_closed[1 - side] = true;
    wait_queue_wake_all(&chan->ends[1 - side].recv_wait);
    wait_queue_wake_all(&chan->ends[1 - side].send_wait);
    struct chan_msg *drain = end->inbox_head;
    end->inbox_head = end->inbox_tail = NULL;
    end->inbox_count = 0;
    chan->refcount--;
    int32_t rc = chan->refcount;

    /* Freeing here, still under the caller's g_sched_lock hold, is safe:
     * kmalloc/kfree serialize on the kheap's OWN lock, never g_sched_lock,
     * so this can't self-deadlock -- it just holds the scheduler lock a
     * little longer on this rare (process-exit) path, which is an
     * acceptable trade against the complexity of deferring frees out of an
     * already-locked caller's critical section. */
    while (drain) {
        struct chan_msg *nx = drain->next;
        chan_msg_free_undelivered(drain);   /* A6: drop in-transit handle refs */
        drain = nx;
    }
    if (rc <= 0) {
        spin_lock(&g_chan_glock); g_chan_live--; spin_unlock(&g_chan_glock);
        kfree(chan);
    }
}

void channel_release_raw_end(struct channel_end *end) {
    uint64_t fl = save_if();
    sched_lock();
    channel_release_raw_end_locked(end);
    sched_unlock();
    restore_if(fl);
}

void channel_release_for_handle_locked(struct process *owner, int handle) {
    struct channel_end *end = (struct channel_end *)owner->obj_handles[handle].obj;
    if (!end) return;
    channel_release_raw_end_locked(end);
}

void channel_release_for_handle(struct process *owner, int handle) {
    struct channel_end *end = (struct channel_end *)owner->obj_handles[handle].obj;
    if (!end) return;
    channel_release_raw_end(end);
}

int channel_close(struct process *caller, int handle) {
    if (handle < 0 || handle >= OBJ_HANDLE_MAX) return -EMBK_EINVAL;
    struct obj_handle *h = &caller->obj_handles[handle];
    if (!h->used || h->kind != HANDLE_KIND_CHANNEL) return -EMBK_EINVAL;
    obj_handle_free(caller, handle);   /* ordinary syscall context: self-locking dispatch */
    return 0;
}

/* ---- move an end to another process (spawn inheritance) ---------------- */
int channel_transfer_end_to(struct process *src, int src_handle, struct process *dst) {
    struct channel_end *end = (struct channel_end *)obj_handle_resolve(src, src_handle, HANDLE_KIND_CHANNEL);
    if (!end) return -EMBK_EINVAL;

    int dh = obj_handle_alloc(dst, HANDLE_KIND_CHANNEL, end);
    if (dh < 0) return dh;

    /* Move: the end stays open (refcount unchanged) -- only the holder
     * changes. Clear the source slot WITHOUT going through channel close. */
    src->obj_handles[src_handle].used = false;
    src->obj_handles[src_handle].kind = HANDLE_KIND_NONE;
    src->obj_handles[src_handle].obj  = 0;
    return dh;
}
