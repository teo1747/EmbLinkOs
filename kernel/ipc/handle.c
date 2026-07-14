/* kernel/ipc/handle.c -- the typed object-handle table (see handle.h). */

#include "ipc/handle.h"
#include "process/process.h"
#include "gfx/surface.h"
#include "ipc/channel.h"
#include "ipc/endpoint.h"
#include "ipc/pipe.h"
#include "include/errno.h"

int obj_handle_alloc(struct process *owner, enum handle_kind k, void *obj) {
    for (int i = 0; i < OBJ_HANDLE_MAX; i++) {
        if (!owner->obj_handles[i].used) {
            owner->obj_handles[i].used      = true;
            owner->obj_handles[i].kind      = k;
            owner->obj_handles[i].obj       = obj;
            owner->obj_handles[i].map_base  = 0;
            owner->obj_handles[i].map_bytes = 0;
            return i;
        }
    }
    return -EMBK_EMFILE;
}

void *obj_handle_resolve(struct process *owner, int handle, enum handle_kind expect) {
    if (handle < 0 || handle >= OBJ_HANDLE_MAX) return 0;
    struct obj_handle *h = &owner->obj_handles[handle];
    if (!h->used || h->kind != expect) return 0;   /* kind check: confused-deputy guard */
    return h->obj;
}

/* `locked`: true iff the caller already holds g_sched_lock. Surface
 * teardown never touches that lock (surfaces use their own per-object lock
 * + vmm_lock), so it's identical either way; channel/endpoint teardown must
 * pick the matching self-locking vs. lock-already-held variant -- see
 * handle.h's comment on obj_handle_free/_locked for why. */
static void obj_handle_free_dispatch(struct process *owner, int handle, bool locked) {
    if (handle < 0 || handle >= OBJ_HANDLE_MAX) return;
    struct obj_handle *h = &owner->obj_handles[handle];
    if (!h->used) return;

    switch (h->kind) {
        case HANDLE_KIND_SURFACE:
            surface_release_for_handle(owner, handle);
            break;
        case HANDLE_KIND_CHANNEL:
            if (locked) channel_release_for_handle_locked(owner, handle);
            else        channel_release_for_handle(owner, handle);
            break;
        case HANDLE_KIND_ENDPOINT:
            if (locked) endpoint_release_for_handle_locked(owner, handle);
            else        endpoint_release_for_handle(owner, handle);
            break;
        case HANDLE_KIND_PIPE:
            if (locked) pipe_release_for_handle_locked(h->obj);
            else        pipe_release_for_handle(h->obj);
            break;
    
        default: break;
    }

    h->used = false; h->kind = HANDLE_KIND_NONE; h->obj = 0;
    h->map_base = 0; h->map_bytes = 0;
}

void obj_handle_free(struct process *owner, int handle) {
    obj_handle_free_dispatch(owner, handle, false);
}
void obj_handle_free_locked(struct process *owner, int handle) {
    obj_handle_free_dispatch(owner, handle, true);
}

void obj_handles_release_all(struct process *owner) {
    /* Called from process_reap_slot(), which ALWAYS holds g_sched_lock --
     * see obj_handle_free_locked's comment (handle.h). */
    for (int i = 0; i < OBJ_HANDLE_MAX; i++) {
        if (owner->obj_handles[i].used) {
            obj_handle_free_locked(owner, i);
        }
    }
}
