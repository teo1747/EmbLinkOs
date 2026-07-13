#ifndef __IPC_ENDPOINT_H__
#define __IPC_ENDPOINT_H__

/* kernel/ipc/endpoint.h -- EmbLink UI Piece 1, Layer B: rendezvous. A
 * listening endpoint lets a server (the compositor) publish itself at a VFS
 * path (e.g. "/run/compositor") so clients can find it by name instead of
 * needing an already-open channel. `chan_connect` mints a fresh connected
 * channel pair per connection: one end goes to the client, the other is
 * queued on the endpoint for the server's `chan_accept` to pick up.
 *
 * Endpoints live only in kernel/fs/epfs.c (a small RAM-backed filesystem
 * mounted at /run) -- they are ephemeral (gone on reboot, gone when the
 * owner dies) and have no business on a persistent COW filesystem. */

#include <stdint.h>
#include "include/types.h"
#include "arch/x86_64/cpu/spinlock.h"
#include "process/process.h"   /* struct wait_queue */
#include "ipc/channel.h"       /* struct channel_end */

struct process;

#define ENDPOINT_ACCEPT_MAX CHAN_QUEUE_MAX   /* server-side ends awaiting accept */

struct listen_endpoint {
    struct process      *owner;        /* who registered it (liveness-checked
                                        * via owner_pid, not dereferenced
                                        * blindly -- see B4's race note) */
    uint32_t              owner_pid;    /* owner->pid AT REGISTRATION time --
                                        * same "recheck, don't trust a stale
                                        * pointer" pattern as struct
                                        * process::parent/parent_pid */
    char                   path[64];    /* the epfs path this was registered
                                        * at, so exit-cleanup can unlink it */
    struct channel_end   *accept_q[ENDPOINT_ACCEPT_MAX]; /* server ends awaiting accept */
    uint32_t              accept_head, accept_count;
    struct wait_queue     accept_wait;  /* server thread(s) blocked in accept() */
    int32_t                refcount;    /* always 0 or 1 in v1: the owner's
                                        * one obj-handle. Kept as a real
                                        * refcount (not a bool) so it composes
                                        * cleanly if that ever changes. */
    bool                   unlinked;    /* true once its epfs node is gone
                                        * (owner exited / listen closed) --
                                        * connect() checks this too, belt-
                                        * and-suspenders with the epfs lookup
                                        * itself returning ENOENT. */
    spinlock_t             lock;
};

/* sys_chan_listen's kernel side: create a listen_endpoint, register it at
 * `path` in epfs, install it as an obj-handle in `owner`. -EMBK_EEXIST if the
 * path is taken. */
int endpoint_listen(struct process *owner, const char *path);

/* sys_chan_accept's kernel side: block until a client connects, then install
 * the popped server-side channel_end as an obj-handle in `caller`. Returns
 * the new CHANNEL handle, or -EMBK_*. */
int endpoint_accept(struct process *caller, int listen_handle);

/* sys_chan_connect's kernel side: resolve `path` in epfs, verify the owner is
 * still alive (B4), mint a channel pair, install end[0] as an obj-handle in
 * `caller`, queue end[1] on the endpoint's accept_q, wake one acceptor.
 * Returns the new CHANNEL handle, or -EMBK_ECONNREFUSED / -EMBK_ENOENT. */
int endpoint_connect(struct process *caller, const char *path);

/* Kind-teardown obj_handle_free()/obj_handle_free_locked() dispatch to for an
 * ENDPOINT slot: unlink the epfs node (B4 -- so a crashed server's path
 * vanishes automatically), release any still-queued (never-accepted)
 * channel ends, free the object. Does NOT clear the handle slot -- the
 * caller does that.
 *
 * Two variants, same "_locked means caller already holds g_sched_lock"
 * convention as channel_release_raw_end/_locked (see that comment,
 * channel.h, for the full reentrant-deadlock reasoning this exists to
 * avoid): the plain version self-locks (ordinary syscall context); the
 * _locked version assumes the lock is already held (process_reap_slot's R2
 * cleanup path). */
void endpoint_release_for_handle(struct process *owner, int handle);          /* self-locking */
void endpoint_release_for_handle_locked(struct process *owner, int handle);   /* caller holds g_sched_lock */

/* Live-endpoint count, for the compositor selftest's R2 full-cleanup check
 * (symmetric with surface_live_count()/channel_live_count()). */
uint32_t endpoint_live_count(void);

#endif /* __IPC_ENDPOINT_H__ */
