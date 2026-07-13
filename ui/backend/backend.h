#ifndef __EMBLINK_UI_BACKEND_H__
#define __EMBLINK_UI_BACKEND_H__

/* ui/backend/backend.h -- EmbLink UI Piece 4a: the render-backend vtable.
 *
 * One immediate pass driven by Piece 3's scene_traverse callback (no deferred
 * command list -- backdrop blur's ordering requirement means it would just
 * replay in the same order anyway). Every Piece-3 node kind maps to one call:
 *   SCENE_NODE_RECT   -> draw_shadow? + draw_backdrop_blur? + draw_rect
 *   SCENE_NODE_IMAGE  -> draw_image
 *   SCENE_NODE_TEXT   -> draw_text  (Piece 4b; declared here so the vtable
 *                                    shape is stable, NULL until 4b lands)
 *   SCENE_NODE_GROUP  -> nothing directly (opacity<1 -> Section 5 offscreen). */

#include <stdint.h>
#include "render_target.h"
#include "scene.h"   /* struct paint, struct color */

/* Rounded-rect clip; the clip stack intersects these. corner_radius 0 = sharp. */
struct clip_rect { float x, y, w, h, corner_radius; };

struct render_backend {
    void (*begin_frame)(struct render_target *rt, const struct clip_rect *dirty_rects, uint32_t n_dirty);
    void (*end_frame)(struct render_target *rt);

    void (*push_clip)(struct render_target *rt, struct clip_rect r);  /* intersects current */
    void (*pop_clip)(struct render_target *rt);

    void (*draw_rect)(struct render_target *rt, float x, float y, float w, float h,
                      float corner_radius, const struct paint *fill, float opacity);
    void (*draw_image)(struct render_target *rt, float x, float y, float w, float h,
                       const void *pixels, uint32_t src_w, uint32_t src_h,
                       uint32_t src_stride, enum embk_pixfmt src_fmt, float opacity);
    void (*draw_shadow)(struct render_target *rt, float x, float y, float w, float h,
                        float corner_radius, float dx, float dy, float blur_radius,
                        struct color color);
    void (*draw_backdrop_blur)(struct render_target *rt, float x, float y, float w, float h,
                               float corner_radius, float blur_radius);
    /* A hairline stroke along the rounded-rect edge, INSIDE the bounds, drawn
     * over the fill. `width` px thick. Antialiased via the same SDF as fills. */
    void (*draw_border)(struct render_target *rt, float x, float y, float w, float h,
                        float corner_radius, float width, struct color color);

    void (*draw_text)(struct render_target *rt, float x, float y, const char *utf8,
                      uint32_t font_handle, float size_px, struct color color, float opacity);
};

/* The CPU backend singleton (its clip stack + scratch pool are module state). */
struct render_backend *cpu_backend_get(void);

/* Scratch-pool access for the Section-5 opacity-group / offscreen path (the
 * driver acquires a target sized to a group's world bounds, renders into it,
 * then blits it back with the group opacity). Growable, no hard cap. */
struct render_target *cpu_scratch_acquire(uint32_t w, uint32_t h);
void                  cpu_scratch_release(struct render_target *rt);

/* Install this frame's dirty rects (Section 6). Coverage in every draw call is
 * then clip-stack (intersect) AND this dirty union. n==0 => full-screen. */
void cpu_set_dirty(const struct clip_rect *rects, uint32_t n);

/* The combined clip-stack x dirty-union coverage at a pixel center. Exposed so
 * the Piece-4b text blit (a separate translation unit) gates its glyph output
 * by the same clip/dirty state every other primitive respects. */
float cpu_coverage_at(float fx, float fy);

/* Clip-stack + dirty-region save/clear/restore -- the driver isolates a group's
 * offscreen render (which draws in the SCRATCH buffer's own coordinate space)
 * from the parent target's active clips AND dirty region, then restores both
 * for the group's blit-back. */
#define CLIP_SAVE_MAX 32
struct clip_saved {
    struct clip_rect rects[CLIP_SAVE_MAX]; int n;
    struct clip_rect dirty_rects[16];      int dirty_n; int dirty_full;
};
void cpu_clip_save_and_clear(struct clip_saved *s);
void cpu_clip_restore(const struct clip_saved *s);

#endif /* __EMBLINK_UI_BACKEND_H__ */
