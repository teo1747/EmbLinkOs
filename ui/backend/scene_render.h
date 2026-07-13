#ifndef __EMBLINK_UI_SCENE_RENDER_H__
#define __EMBLINK_UI_SCENE_RENDER_H__

/* ui/backend/scene_render.h -- EmbLink UI Piece 4a: the traversal DRIVER.
 *
 * Bridges Piece 3's scene tree to the render_backend vtable: maps each node
 * kind to its draw call, does grouped-opacity offscreen compositing (Section
 * 5), and runs the dirty-rect algorithm (Section 6 -- the crux) so per-pixel
 * work stays bounded to what actually changed while the tree walk itself
 * covers everything. This is the same machinery a single client uses for its
 * own window AND the compositor uses to composite every window (Section 7):
 * no separate multi-window path, just this tree, one level up. */

#include "backend.h"
#include "scene.h"

struct rect_cache { float x, y, w, h; uint32_t generation; int valid; };

struct scene_renderer {
    struct render_backend *be;
    struct rect_cache     *cache;      /* per-node world rect from last frame */
    uint32_t               cache_cap;
    struct clip_rect       dirty[16];  /* this frame's accumulator (Section 6) */
    int                    n_dirty;
    int                    full;       /* cap exceeded -> one full-screen rect */
};

void scene_render_init(struct scene_renderer *r, struct render_backend *be);
void scene_render_destroy(struct scene_renderer *r);

/* Render one frame of `root`'s tree into `target`, repainting only dirty
 * regions (everything on the first frame, since nothing is cached yet). */
void scene_render_frame(struct scene_renderer *r, struct scene_arena *a,
                        struct node_handle root, struct render_target *target);

#endif /* __EMBLINK_UI_SCENE_RENDER_H__ */
