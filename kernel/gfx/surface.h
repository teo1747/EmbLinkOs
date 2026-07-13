#ifndef __GFX_SURFACE_H__
#define __GFX_SURFACE_H__

/* kernel/gfx/surface.h -- EmbLink UI Piece 1: the surface & shared-memory
 * model. A SURFACE is a kernel-owned, refcounted run of physical pages that
 * two processes (a UI client and the compositor) both map, at their own
 * virtual addresses, to share pixels with zero copies. It is the kernel's
 * first cross-address-space shared-memory primitive.
 *
 * A surface is named by a typed HANDLE (obj_handle), not an fd and not a raw
 * id -- consistent with the capability-handle model already used for pids
 * (process_handle_*). See the design spec (emblink-ui-01-surface.md). */

#include <stdint.h>
#include "include/types.h"
#include "arch/x86_64/cpu/spinlock.h"
#include "ipc/handle.h"   /* the shared obj_handle table (enum handle_kind,
                           * struct obj_handle, OBJ_HANDLE_MAX,
                           * USER_SHARED_VA_BASE, obj_handle_* API) */

struct process;   /* forward decl -- surface.h must NOT include process.h. */

/* ---- pixel formats (ABI-visible; only one implemented) ------------------ */
enum embk_pixfmt {
    EMBK_PIXFMT_NONE         = 0,
    EMBK_PIXFMT_BGRA8888_PRE = 1,  /* premultiplied alpha; the ONLY impl'd one */
    EMBK_PIXFMT_RGBA8888_PRE = 2,  /* reserved, not implemented */
    EMBK_PIXFMT_XRGB8888     = 3,  /* reserved, not implemented */
};

/* ---- the surface object ------------------------------------------------- */
#define SURFACE_MIN_BUFFERS 2
#define SURFACE_MAX_BUFFERS 4

enum buffer_owner {
    BUF_OWNER_CLIENT = 0,   /* client may draw into it */
    BUF_OWNER_COMPOSITOR,   /* compositor is (or may be) sampling it */
};

struct surface_buffer {
    uint64_t         *pages;      /* physical page addresses backing this buffer */
    uint32_t          n_pages;
    enum buffer_owner owner;
    bool              committed;  /* client has posted it as the newest frame */
    uint64_t          seq;        /* commit sequence #, for "newest wins" (B3) */
};

struct surface {
    uint32_t          width, height;
    uint32_t          stride;     /* bytes per row = width*4 (page-aligned buffer) */
    enum embk_pixfmt  format;
    uint32_t          n_buffers;
    uint64_t          buffer_size; /* bytes per buffer, whole pages */

    struct surface_buffer buffers[SURFACE_MAX_BUFFERS];

    int32_t           refcount;   /* # of processes with it mapped; freed at 0 */
    uint32_t          id;         /* global, for logging/debug only */
    spinlock_t        lock;       /* guards owner/committed/seq transitions */
};

/* Layout handed to userland so both sides agree without re-querying
 * (copy_to_user'd by surface_create/map). */
struct surface_info {
    uint32_t width, height, stride, format, n_buffers;
    uint64_t buffer_size;
};

/* ---- surface operations (the kernel side of the syscalls) --------------- */
/* All return >= 0 / a handle / 0 on success, or a negative -EMBK_* code. */
int  surface_create(struct process *client, uint32_t w, uint32_t h,
                    uint32_t format, uint32_t n_buffers, struct surface_info *out_info);
uint64_t surface_map(struct process *caller, int handle, struct surface_info *out_info);
int  surface_acquire(struct process *caller, int handle);
int  surface_commit(struct process *caller, int handle, int buffer_idx);
int  surface_release(struct process *caller, int handle, int buffer_idx);
int  surface_destroy(struct process *caller, int handle);

/* Minimal handle_transfer for Piece 1: dup `src_handle` (a surface the parent
 * holds) into `dst`'s obj_handle table AND map it into dst's address space.
 * Used by the SPAWN_ACTION_INHERIT_SURFACE spawn action so a spawned child is
 * born already sharing the parent's surface -- the honest cross-address-space
 * path the S1 selftest needs, without waiting for Piece 2's protocol. Returns
 * the child's new handle, or -EMBK_*. */
int  surface_transfer_to(struct process *src, int src_handle, struct process *dst);

/* Live-surface count, for the crash-safety selftest (S4) to prove a surface
 * is actually freed when its refcount hits 0. */
uint32_t surface_live_count(void);

/* Kind-specific teardown, called by obj_handle_free() (kernel/ipc/handle.c)
 * when a HANDLE_KIND_SURFACE slot is released: unmap the surface from `owner`
 * (if mapped) and drop its refcount (freeing the object at 0). Does NOT clear
 * the handle slot -- obj_handle_free does that. */
void surface_release_for_handle(struct process *owner, int handle);

/* Refcount get/put by object pointer, for the channel ancillary-handle path
 * (kernel/ipc/channel.c): a surface handle passed COPY over a channel bumps
 * the ref (held in-transit until the receiver installs its own handle);
 * put drops it (freeing the surface at 0). surface refcount counts
 * outstanding references (handles + in-transit), not mappings. */
struct surface;
void surface_ref_get(struct surface *s);
void surface_ref_put(struct surface *s);

/* Unmap the surface named by `owner`'s handle from its address space WITHOUT
 * dropping the refcount -- used when a surface handle is MOVED off a channel
 * (the sender relinquishes its mapping; the ref travels to the receiver). */
void surface_unmap_from_handle(struct process *owner, int handle);

#endif /* __GFX_SURFACE_H__ */
