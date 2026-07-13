/* kernel/gfx/surface.c -- EmbLink UI Piece 1: surfaces & shared memory.
 * See surface.h and the design spec (emblink-ui-01-surface.md) for the model,
 * invariants (H1, B1-B3, R1-R3) and the proving selftests (S1-S4). */

#include "gfx/surface.h"
#include "process/process.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "include/kmalloc.h"
#include "include/kstring.h"
#include "include/errno.h"
#include "include/kprintf.h"

/* Sanity cap on surface dimensions (reject absurd allocations). */
#define SURFACE_MAX_DIM 8192

static uint64_t g_surface_seq;      /* global commit sequence source (B3) */
static uint32_t g_surface_next_id;  /* debug id source */
static int32_t  g_surface_live;     /* live-surface count, for S4 */
static spinlock_t g_surface_glock;  /* guards the three globals above */

uint32_t surface_live_count(void) { return (uint32_t)g_surface_live; }

/* ====================================================================== */
/* The obj_handle table itself lives in kernel/ipc/handle.c now (shared by    */
/* surfaces, channels, endpoints). This file provides the surface-kind        */
/* teardown obj_handle_free() dispatches to (surface_release_for_handle).     */
/* ====================================================================== */

/* ====================================================================== */
/* Physical-page / mapping helpers                                        */
/* ====================================================================== */

static void surface_free_object(struct surface *surf);          /* fwd */
static void surface_free_object_partial(struct surface *surf);  /* fwd (create error path) */

/* Map every buffer of `surf` into `proc`'s address space at a freshly
 * bump-allocated shared-region VA. Returns the base VA (buffer 0), or 0 on
 * failure. Records nothing in the handle -- caller stores map_base/map_bytes. */
static uint64_t surface_map_pages_into(struct process *proc, struct surface *surf) {
    uint64_t total = (uint64_t)surf->n_buffers * surf->buffer_size;
    uint64_t base  = proc->shared_next_va;

    uint64_t va = base;
    for (uint32_t b = 0; b < surf->n_buffers; b++) {
        struct surface_buffer *buf = &surf->buffers[b];
        for (uint32_t p = 0; p < buf->n_pages; p++) {
            /* VMM_USER|WRITABLE: both sides read+write pixels. VMM_NX: pixels
             * are data, never executed. */
            if (vmm_map_in(proc->pml4_phys, va, buf->pages[p],
                           VMM_USER | VMM_WRITABLE | VMM_NX) < 0) {
                /* Unwind whatever we mapped so far, then fail. */
                for (uint64_t u = base; u < va; u += PAGE_SIZE) {
                    vmm_unmap_in(proc->pml4_phys, u);
                }
                return 0;
            }
            va += PAGE_SIZE;
        }
    }

    proc->shared_next_va = base + total;   /* bump; VA space isn't reclaimed */
    return base;
}

static void surface_unmap_pages_from(struct process *proc, uint64_t base, uint64_t bytes) {
    for (uint64_t va = base; va < base + bytes; va += PAGE_SIZE) {
        vmm_unmap_in(proc->pml4_phys, va);   /* clears PTE, does NOT free the frame */
    }
}

/* Drop one reference. When it hits 0, free the physical pages and the object
 * (R1). Caller must NOT hold surf->lock. */
static void surface_unref(struct surface *surf) {
    spin_lock(&surf->lock);
    surf->refcount--;
    int32_t rc = surf->refcount;
    spin_unlock(&surf->lock);
    if (rc <= 0) {
        surface_free_object(surf);
    }
}

static void surface_free_object(struct surface *surf) {
    for (uint32_t b = 0; b < surf->n_buffers; b++) {
        struct surface_buffer *buf = &surf->buffers[b];
        for (uint32_t p = 0; p < buf->n_pages; p++) {
            pmm_free_page(buf->pages[p]);
        }
        if (buf->pages) kfree(buf->pages);
    }
    spin_lock(&g_surface_glock);
    g_surface_live--;
    spin_unlock(&g_surface_glock);
    kfree(surf);
}

/* ====================================================================== */
/* surface_create                                                         */
/* ====================================================================== */

int surface_create(struct process *client, uint32_t w, uint32_t h,
                   uint32_t format, uint32_t n_buffers, struct surface_info *out_info) {
    if (format != EMBK_PIXFMT_BGRA8888_PRE) {
        return -EMBK_ENOTSUP;   /* only the premultiplied-BGRA format is impl'd */
    }
    if (w == 0 || h == 0 || w > SURFACE_MAX_DIM || h > SURFACE_MAX_DIM) {
        return -EMBK_EINVAL;
    }
    if (n_buffers < SURFACE_MIN_BUFFERS || n_buffers > SURFACE_MAX_BUFFERS) {
        return -EMBK_EINVAL;
    }

    uint32_t stride = w * 4;                                   /* BGRA = 4 bpp */
    uint64_t bufbytes = (uint64_t)stride * h;
    uint64_t buf_pages = (bufbytes + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t buffer_size = buf_pages * PAGE_SIZE;              /* whole pages */

    struct surface *surf = (struct surface *)kmalloc(sizeof(*surf));
    if (!surf) return -EMBK_ENOMEM;
    memset(surf, 0, sizeof(*surf));
    surf->width = w; surf->height = h; surf->stride = stride;
    surf->format = (enum embk_pixfmt)format;
    surf->n_buffers = n_buffers;
    surf->buffer_size = buffer_size;
    surf->refcount = 0;
    spinlock_init(&surf->lock);

    /* Allocate all N buffers up front -- no per-frame allocation on the hot
     * path. Any failure frees everything already allocated (no half-built
     * surface ever escapes). */
    for (uint32_t b = 0; b < n_buffers; b++) {
        struct surface_buffer *buf = &surf->buffers[b];
        buf->pages = (uint64_t *)kmalloc(buf_pages * sizeof(uint64_t));
        if (!buf->pages) { surf->n_buffers = b; surface_free_object_partial(surf); return -EMBK_ENOMEM; }
        buf->n_pages = 0;
        for (uint64_t p = 0; p < buf_pages; p++) {
            uint64_t phys = pmm_alloc_page();
            if (!phys) { surf->n_buffers = b + 1; surface_free_object_partial(surf); return -EMBK_ENOMEM; }
            /* zero the frame so a fresh surface starts transparent */
            memset((void *)P2V(phys), 0, PAGE_SIZE);
            buf->pages[p] = phys;
            buf->n_pages++;
        }
        buf->owner = BUF_OWNER_CLIENT;   /* client draws first */
        buf->committed = false;
        buf->seq = 0;
    }

    spin_lock(&g_surface_glock);
    surf->id = ++g_surface_next_id;
    g_surface_live++;
    spin_unlock(&g_surface_glock);

    /* Map into the client and hand it a handle (refcount -> 1). */
    uint64_t base = surface_map_pages_into(client, surf);
    if (!base) { surface_free_object(surf); return -EMBK_ENOMEM; }

    int handle = obj_handle_alloc(client, HANDLE_KIND_SURFACE, surf);
    if (handle < 0) {
        surface_unmap_pages_from(client, base, (uint64_t)n_buffers * buffer_size);
        surface_free_object(surf);
        return handle;
    }
    client->obj_handles[handle].map_base  = base;
    client->obj_handles[handle].map_bytes = (uint64_t)n_buffers * buffer_size;
    surf->refcount = 1;

    if (out_info) {
        out_info->width = w; out_info->height = h; out_info->stride = stride;
        out_info->format = format; out_info->n_buffers = n_buffers;
        out_info->buffer_size = buffer_size;
    }
    kprintf("surface: created id=%u %ux%u %u buf(s), handle=%d (live=%d)\n",
            (unsigned)surf->id, (unsigned)w, (unsigned)h, (unsigned)n_buffers,
            handle, (int)g_surface_live);
    return handle;
}

/* Partial-free used only on the create error path: frees the b buffers whose
 * pages arrays are allocated so far (surf->n_buffers was set to the count to
 * free), plus the surface + live-count already bumped? No -- create bumps
 * live AFTER buffers succeed, so this must NOT touch live. Kept separate from
 * surface_free_object (which does decrement live). */
static void surface_free_object_partial(struct surface *surf) {
    for (uint32_t b = 0; b < surf->n_buffers; b++) {
        struct surface_buffer *buf = &surf->buffers[b];
        for (uint32_t p = 0; p < buf->n_pages; p++) pmm_free_page(buf->pages[p]);
        if (buf->pages) kfree(buf->pages);
    }
    kfree(surf);
}

/* ====================================================================== */
/* surface_map (map a surface this process already holds a handle to)      */
/* ====================================================================== */

uint64_t surface_map(struct process *caller, int handle, struct surface_info *out_info) {
    struct surface *surf = (struct surface *)obj_handle_resolve(caller, handle, HANDLE_KIND_SURFACE);
    if (!surf) return 0;

    struct obj_handle *h = &caller->obj_handles[handle];
    uint64_t base = h->map_base;
    if (base == 0) {
        /* Not mapped into THIS address space yet -- map now. Deliberately
         * does NOT bump refcount here: obj_handle_alloc() never counts a
         * reference itself (handle.h's contract), so by the time a handle
         * exists in `caller`'s table at all, whatever installed it already
         * accounted for exactly one reference -- surface_create (its own
         * initial refcount=1), surface_transfer_to (spawn-inherit, its own
         * explicit surface_ref_get), or a channel COPY/MOVE recv (the ref
         * travels with the ancillary handle, see channel.c's top comment).
         * This is purely "give me the pages mapped", the step a channel
         * recv leaves for the receiver to do separately (spec C.4) --
         * bumping refcount again here would double-count that one
         * reference and leak the surface (it would never reach 0). */
        base = surface_map_pages_into(caller, surf);
        if (!base) return 0;
        h->map_base  = base;
        h->map_bytes = (uint64_t)surf->n_buffers * surf->buffer_size;
    }

    if (out_info) {
        out_info->width = surf->width; out_info->height = surf->height;
        out_info->stride = surf->stride; out_info->format = surf->format;
        out_info->n_buffers = surf->n_buffers; out_info->buffer_size = surf->buffer_size;
    }
    return base;
}

/* ====================================================================== */
/* ownership state machine: acquire / commit / release (B1-B3)            */
/* ====================================================================== */

int surface_acquire(struct process *caller, int handle) {
    struct surface *surf = (struct surface *)obj_handle_resolve(caller, handle, HANDLE_KIND_SURFACE);
    if (!surf) return -EMBK_EINVAL;

    spin_lock(&surf->lock);
    int idx = -EMBK_EAGAIN;   /* B2: all buffers held by compositor -> starve */
    for (uint32_t b = 0; b < surf->n_buffers; b++) {
        if (surf->buffers[b].owner == BUF_OWNER_CLIENT) { idx = (int)b; break; }
    }
    spin_unlock(&surf->lock);
    return idx;
}

int surface_commit(struct process *caller, int handle, int buffer_idx) {
    struct surface *surf = (struct surface *)obj_handle_resolve(caller, handle, HANDLE_KIND_SURFACE);
    if (!surf) return -EMBK_EINVAL;
    if (buffer_idx < 0 || (uint32_t)buffer_idx >= surf->n_buffers) return -EMBK_EINVAL;

    spin_lock(&surf->lock);
    struct surface_buffer *buf = &surf->buffers[buffer_idx];
    if (buf->owner != BUF_OWNER_CLIENT) {   /* B1: can't commit what you don't own */
        spin_unlock(&surf->lock);
        return -EMBK_EINVAL;
    }
    spin_lock(&g_surface_glock);
    buf->seq = ++g_surface_seq;
    spin_unlock(&g_surface_glock);
    buf->committed = true;
    buf->owner = BUF_OWNER_COMPOSITOR;      /* client -> compositor */
    spin_unlock(&surf->lock);
    /* Piece 2 will wake a compositor thread blocked on a frame event here. */
    return 0;
}

int surface_release(struct process *caller, int handle, int buffer_idx) {
    struct surface *surf = (struct surface *)obj_handle_resolve(caller, handle, HANDLE_KIND_SURFACE);
    if (!surf) return -EMBK_EINVAL;
    if (buffer_idx < 0 || (uint32_t)buffer_idx >= surf->n_buffers) return -EMBK_EINVAL;

    spin_lock(&surf->lock);
    struct surface_buffer *buf = &surf->buffers[buffer_idx];
    if (buf->owner != BUF_OWNER_COMPOSITOR) {   /* only the compositor releases */
        spin_unlock(&surf->lock);
        return -EMBK_EINVAL;
    }
    buf->owner = BUF_OWNER_CLIENT;              /* compositor -> client */
    buf->committed = false;
    spin_unlock(&surf->lock);
    return 0;
}

/* ====================================================================== */
/* destroy / transfer / crash-safety cleanup                              */
/* ====================================================================== */

/* The surface-kind teardown obj_handle_free() (kernel/ipc/handle.c) calls:
 * unmap the surface from `owner` (if mapped) and drop its refcount (freeing
 * the object at 0). Does NOT clear the handle slot -- obj_handle_free does. */
void surface_release_for_handle(struct process *owner, int handle) {
    struct obj_handle *h = &owner->obj_handles[handle];
    struct surface *surf = (struct surface *)h->obj;
    if (h->map_base) {
        surface_unmap_pages_from(owner, h->map_base, h->map_bytes);
    }
    surface_unref(surf);
}

/* Refcount get/put by pointer, for the channel ancillary-handle path. */
void surface_ref_get(struct surface *s) {
    spin_lock(&s->lock);
    s->refcount++;
    spin_unlock(&s->lock);
}
void surface_ref_put(struct surface *s) {
    surface_unref(s);   /* drops one ref; frees the object + pages at 0 */
}

/* Unmap the surface from `owner`'s address space without dropping the ref
 * (the ref travels with the moved handle). Clears the handle's mapping
 * record so a later exit-cleanup won't try to unmap it again. */
void surface_unmap_from_handle(struct process *owner, int handle) {
    struct obj_handle *h = &owner->obj_handles[handle];
    if (h->map_base) {
        surface_unmap_pages_from(owner, h->map_base, h->map_bytes);
        h->map_base = 0;
        h->map_bytes = 0;
    }
}

/* Public destroy syscall: validate it's a surface handle, then route through
 * obj_handle_free (dispatches to surface_release_for_handle + clears slot). */
int surface_destroy(struct process *caller, int handle) {
    if (handle < 0 || handle >= OBJ_HANDLE_MAX) return -EMBK_EINVAL;
    struct obj_handle *h = &caller->obj_handles[handle];
    if (!h->used || h->kind != HANDLE_KIND_SURFACE) return -EMBK_EINVAL;
    obj_handle_free(caller, handle);
    return 0;
}

int surface_transfer_to(struct process *src, int src_handle, struct process *dst) {
    struct surface *surf = (struct surface *)obj_handle_resolve(src, src_handle, HANDLE_KIND_SURFACE);
    if (!surf) return -EMBK_EINVAL;

    int dh = obj_handle_alloc(dst, HANDLE_KIND_SURFACE, surf);
    if (dh < 0) return dh;

    uint64_t base = surface_map_pages_into(dst, surf);
    if (!base) {
        dst->obj_handles[dh].used = false;
        dst->obj_handles[dh].kind = HANDLE_KIND_NONE;
        dst->obj_handles[dh].obj = NULL;
        return -EMBK_ENOMEM;
    }
    dst->obj_handles[dh].map_base  = base;
    dst->obj_handles[dh].map_bytes = (uint64_t)surf->n_buffers * surf->buffer_size;
    spin_lock(&surf->lock);
    surf->refcount++;
    spin_unlock(&surf->lock);
    return dh;
}
