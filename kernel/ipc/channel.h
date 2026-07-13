#ifndef __IPC_CHANNEL_H__
#define __IPC_CHANNEL_H__

/* kernel/ipc/channel.h -- EmbLink UI Piece 1, Layer A: a general, reusable,
 * message-oriented IPC channel. A channel is a bidirectional endpoint pair;
 * each end is held by one process via an obj-handle. A message is a
 * variable-length byte payload plus 0..N ancillary handles, delivered
 * atomically with datagram/SEQPACKET boundaries (one send -> one recv).
 *
 * The compositor is the first customer (it carries surface handles between
 * client and compositor via the copy/move ancillary-handle mechanism), but
 * the primitive is fully general. See the design spec (Layer A). */

#include <stdint.h>
#include "include/types.h"
#include "arch/x86_64/cpu/spinlock.h"
#include "process/process.h"   /* struct wait_queue */
#include "ipc/handle.h"        /* enum handle_kind */

#define CHAN_MSG_MAX_BYTES    4096   /* per-message payload cap */
#define CHAN_MSG_MAX_HANDLES     8   /* ancillary handles per message */
#define CHAN_QUEUE_MAX          32   /* undelivered msgs buffered per end */

/* per-handle transfer selection (parallel flags array on send) */
#define CHAN_HANDLE_COPY 0   /* sender keeps its handle; receiver gets one too */
#define CHAN_HANDLE_MOVE 1   /* sender's handle is consumed; capability moves */

struct chan_handle_ref {
    enum handle_kind kind;
    void            *obj;      /* the referenced object; its ref is held in-transit */
    bool             is_move;
};

struct chan_msg {
    struct chan_msg      *next;    /* intrusive inbox link */
    uint32_t              len;
    uint8_t              *bytes;   /* kernel copy of the payload (NULL if len==0) */
    uint32_t              n_handles;
    struct chan_handle_ref handles[CHAN_MSG_MAX_HANDLES];
};

struct channel;

struct channel_end {
    struct channel      *chan;
    int                  side;                 /* 0 or 1 */
    struct chan_msg     *inbox_head, *inbox_tail;  /* msgs waiting for THIS end */
    uint32_t             inbox_count;
    struct wait_queue    recv_wait;            /* threads blocked recv-ing here */
    struct wait_queue    send_wait;            /* senders blocked: this inbox full */
};

struct channel {
    struct channel_end   ends[2];
    int32_t              refcount;      /* # of ends still OPEN (held by a handle) */
    bool                 peer_closed[2]; /* peer_closed[i] == ends[i]'s peer hung up */
    uint32_t             id;
    spinlock_t           lock;          /* the message queues + blocking are
                                         * serialized under g_sched_lock (see
                                         * channel.c); this guards nothing hot */
};

/* Create a connected pair; install BOTH ends as obj-handles in `owner`,
 * writing the two handle ints. Bootstraps parent/child channels (the parent
 * passes one end to a child). Returns 0 or -EMBK_*. */
int channel_create_pair(struct process *owner, int *out_h0, int *out_h1);

/* Create a connected channel WITHOUT installing any handles or bumping
 * refcount -- Layer B's connect/accept rendezvous (kernel/ipc/endpoint.c)
 * installs the ends itself: end[0] as a real handle in the connecting
 * client (refcount++ then), end[1] queued on the listening endpoint's
 * accept_q as an "in-transit" reference (refcount++ then too -- exactly the
 * same in-transit-ref shape a channel message's ancillary handle already
 * uses, see A6) until accept() converts it into a real handle. Returns NULL
 * on OOM. */
struct channel *channel_create_raw(void);
/* Explicit refcount ops for the raw-channel path above (channel_send/recv's
 * ancillary-handle bookkeeping does this implicitly; connect/accept must do
 * it by hand since they're installing/queuing ENDS, not ancillary handles). */
void channel_ref_get(struct channel *chan);

/* Release one channel_end directly (not via a process's obj_handle table) --
 * for a raw end still sitting in a listen_endpoint's accept_q when the
 * endpoint is torn down before anyone accepted it: marks its peer closed,
 * drains its inbox (releasing any in-transit ancillary refs, A6), and drops
 * the channel's refcount (freeing it at 0). channel_release_for_handle is a
 * thin wrapper over this for the handle-owned case.
 *
 * Two variants, same "_locked means caller already holds g_sched_lock"
 * convention schedule()/schedule_locked() already use in process.c:
 * process_reap_slot() (process.c) -- which obj_handles_release_all() (R2
 * cleanup) runs inside -- is ALWAYS called with g_sched_lock already held
 * (every existing call site does; it's a structural invariant of this
 * scheduler). The self-locking public version below would deadlock
 * (spin_lock is not reentrant) if called from that path -- which is exactly
 * what happened the first time a selftest relied on R2 to close a channel
 * end automatically instead of an explicit sys_chan_close() (ordinary
 * syscall context, lock NOT held, where the self-locking version is
 * correct). */
void channel_release_raw_end(struct channel_end *end);          /* self-locking */
void channel_release_raw_end_locked(struct channel_end *end);   /* caller holds g_sched_lock */

/* Locked twin of channel_release_for_handle, for obj_handle_free_locked()
 * (kernel/ipc/handle.c's R2 dispatch) to call from inside process_reap_slot. */
void channel_release_for_handle_locked(struct process *owner, int handle);

/* Kernel-side send/recv -- the syscall layer does the user<->kernel copies and
 * passes kernel buffers. hnds[]/flags[] are the caller's handle ints and the
 * per-handle COPY/MOVE selection. Blocks on backpressure (send) / empty inbox
 * (recv) per invariants A2/A3. Returns 0 or -EMBK_* (EPIPE on peer close,
 * EMSGSIZE if recv's buffer is smaller than the message). */
int64_t channel_send(struct process *caller, int handle,
                     const void *bytes, uint32_t len,
                     const int *hnds, const int *flags, uint32_t n_hnd);
int64_t channel_recv(struct process *caller, int handle,
                     void *kbuf, uint32_t buflen,
                     uint32_t *out_len, int *out_hnds, uint32_t *out_nhnd);

/* Public close syscall: drop the caller's end. */
int  channel_close(struct process *caller, int handle);

/* Kind-teardown obj_handle_free() dispatches to for a CHANNEL slot: close the
 * end, set peer_closed + wake the peer, release any msgs still queued for this
 * end (A6), free the channel at refcount 0. Does NOT clear the slot. */
void channel_release_for_handle(struct process *owner, int handle);

/* MOVE a channel end (`src_handle`) from `src` to `dst` -- the channel stays
 * open (refcount unchanged), only the holder changes. For
 * SPAWN_ACTION_INHERIT_CHANNEL (parent hands a child one end). Returns the
 * child's new handle, or -EMBK_*. */
int  channel_transfer_end_to(struct process *src, int src_handle, struct process *dst);

/* Live-channel count, for the handle-refcount selftest (S-chan-3). */
uint32_t channel_live_count(void);

#endif /* __IPC_CHANNEL_H__ */
