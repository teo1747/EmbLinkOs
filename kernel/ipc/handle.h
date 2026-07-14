#ifndef __IPC_HANDLE_H__
#define __IPC_HANDLE_H__

/* kernel/ipc/handle.h -- the per-process TYPED OBJECT HANDLE table, shared
 * infrastructure for every structural kernel object a ring-3 process names by
 * capability rather than by an ambient id: surfaces (kernel/gfx), channel
 * ends and listening endpoints (kernel/ipc). Distinct from the pid
 * `proc_handle` table (which maps a handle -> pid for wait/kill); this maps a
 * handle -> a live refcounted kernel object pointer.
 *
 * EmbLink UI Piece 1, Layer C.1: one table, many kinds. The kind check on
 * resolve is load-bearing -- it stops a channel handle being used where a
 * surface is expected, etc. */

#include <stdint.h>
#include "include/types.h"

struct process;   /* forward decl -- handle.h has no dependency on process.h;
                   * process.h includes THIS for the obj_handles[] field. */

enum handle_kind {
    HANDLE_KIND_NONE = 0,
    HANDLE_KIND_SURFACE,    /* struct surface *      (kernel/gfx/surface.h) */
    HANDLE_KIND_CHANNEL,    /* struct channel_end *  (kernel/ipc/channel.h) */
    HANDLE_KIND_ENDPOINT,   /* struct listen_endpoint * (kernel/ipc/endpoint.h) */
    HANDLE_KIND_PIPE,       /* struct pipe_end *     (kernel/ipc/pipe.h) */
};

struct obj_handle {
    enum handle_kind kind;
    void            *obj;
    /* Surface-only: the user VA this process mapped the surface at, so
     * exit/free can unmap it. 0 for kinds that aren't memory-mapped. */
    uint64_t         map_base;
    uint64_t         map_bytes;
    bool             used;
};

#define OBJ_HANDLE_MAX 32   /* per process; distinct from PROC_HANDLE_MAX */

/* Base of the per-process VA window surfaces map into (slot 5, between heap
 * slot 6 and code): each mapping bump-allocates from proc->shared_next_va. */
#define USER_SHARED_VA_BASE 0x0000500000000000ULL

/* Allocate a free slot naming `obj` of kind `k`. Does NOT itself bump the
 * object's refcount -- the caller (surface_create, channel install, ...)
 * owns that, since the "one ref per handle" accounting is kind-specific.
 * Returns handle >= 0, or -EMBK_EMFILE if the table is full. */
int   obj_handle_alloc(struct process *owner, enum handle_kind k, void *obj);

/* Resolve a handle to its object, or NULL if out-of-range / unused / WRONG
 * KIND. The kind check is the confused-deputy guard. */
void *obj_handle_resolve(struct process *owner, int handle, enum handle_kind expect);

/* Release a handle: dispatch to the kind-specific teardown (surface: unmap +
 * unref; channel: close end + wake peer; endpoint: unregister + remove VFS
 * node), then clear the slot. Idempotent on an unused handle.
 *
 * Two variants: the plain one self-locks where its kind-specific teardown
 * needs g_sched_lock (channel/endpoint do; surface never touches it). The
 * _locked one assumes the caller ALREADY holds g_sched_lock -- required by
 * obj_handles_release_all() below, since process_reap_slot() (process.c),
 * which R2 cleanup runs inside, is ALWAYS called with that lock already
 * held (a structural invariant of this scheduler, every existing call site
 * does it). Calling the self-locking version from there would reenter a
 * non-reentrant spinlock -- see channel_release_raw_end/_locked's comment
 * (channel.h) for the full story; this is the dispatch point that has to
 * pick the right one. */
void  obj_handle_free(struct process *owner, int handle);          /* self-locking */
void  obj_handle_free_locked(struct process *owner, int handle);   /* caller holds g_sched_lock */

/* Release EVERY handle a (dying) process holds. Called from
 * process_reap_slot BEFORE the address space is torn down (R2 crash-safety),
 * so shared surface pages are unmapped (not freed) and channel peers are
 * woken with EPIPE. Uses obj_handle_free_locked() -- see its comment. */
void  obj_handles_release_all(struct process *owner);

#endif /* __IPC_HANDLE_H__ */
