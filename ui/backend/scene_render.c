/* ui/backend/scene_render.c -- EmbLink UI Piece 4a: the traversal driver
 * (see scene_render.h). Implements Section 5 (grouped opacity) and Section 6
 * (the dirty-rect algorithm, specified as explicit steps precisely because
 * it's easy to get subtly wrong). */

#include "scene_render.h"
#include <stdlib.h>
#include <math.h>


/* ------------------------------------------------------------------------- */
/* geometry helpers                                                          */
/* ------------------------------------------------------------------------- */

struct frect { float x, y, w, h; };   /* w<=0 || h<=0 == empty */

static struct frect frect_empty(void) { struct frect f = {0,0,0,0}; return f; }
static int frect_is_empty(struct frect f) { return f.w <= 0 || f.h <= 0; }

static struct frect frect_union(struct frect a, struct frect b) {
    if (frect_is_empty(a)) return b;
    if (frect_is_empty(b)) return a;
    float x0 = a.x < b.x ? a.x : b.x;
    float y0 = a.y < b.y ? a.y : b.y;
    float x1 = (a.x + a.w) > (b.x + b.w) ? (a.x + a.w) : (b.x + b.w);
    float y1 = (a.y + a.h) > (b.y + b.h) ? (a.y + a.h) : (b.y + b.h);
    struct frect f = { x0, y0, x1 - x0, y1 - y0 };
    return f;
}
static int frect_overlap(struct frect a, struct frect b) {
    if (frect_is_empty(a) || frect_is_empty(b)) return 0;
    return !(a.x + a.w <= b.x || b.x + b.w <= a.x ||
             a.y + a.h <= b.y || b.y + b.h <= a.y);
}

/* World-space axis-aligned bounding box of a node's local (0,0,w,h) rect. For
 * pure translate+scale this is exact; for rotation it's the rotated rect's
 * AABB (fine for the CPU backend, which draws axis-aligned rects). */
static struct frect node_world_aabb(const struct scene_node *n, const float world[16]) {
    float cx[4] = { 0, n->width, 0, n->width };
    float cy[4] = { 0, 0, n->height, n->height };
    float minx = 1e30f, miny = 1e30f, maxx = -1e30f, maxy = -1e30f;
    for (int i = 0; i < 4; i++) {
        float X = world[0] * cx[i] + world[4] * cy[i] + world[12];
        float Y = world[1] * cx[i] + world[5] * cy[i] + world[13];
        if (X < minx) minx = X;
        if (X > maxx) maxx = X;
        if (Y < miny) miny = Y;
        if (Y > maxy) maxy = Y;
    }
    struct frect f = { minx, miny, maxx - minx, maxy - miny };
    return f;
}

/* Union of a whole subtree's world AABBs (Section 5's group bounding rect). */
static int g_sb_depth;
static struct frect subtree_bounds(struct scene_arena *a, struct node_handle h,
                                   const float wparent[16]) {
    struct scene_node *n = scene_resolve(a, h);
    if (!n || g_sb_depth > 1024) return frect_empty();
    g_sb_depth++;
    float local[16], world[16];
    scene_trs_to_matrix(n, local);
    scene_mat4_mul(wparent, local, world);
    struct frect b = node_world_aabb(n, world);
    struct node_handle c = n->first_child;
    uint32_t guard = 512;   /* cap sibling walks (cycle-safe) */
    while (!node_handle_is_null(c) && guard-- != 0) {
        struct scene_node *cn = scene_resolve(a, c);
        struct node_handle next = cn ? cn->next_sibling : NODE_HANDLE_NULL;
        b = frect_union(b, subtree_bounds(a, c, world));
        c = next;
    }
    g_sb_depth--;
    return b;
}

/* ------------------------------------------------------------------------- */
/* dirty-rect accumulation (Section 6)                                       */
/* ------------------------------------------------------------------------- */

static void dirty_add(struct scene_renderer *r, struct frect f) {
    if (r->full || frect_is_empty(f)) return;
    if (r->n_dirty >= 16) { r->full = 1; return; }   /* Section 0 cap -> full-screen */
    struct clip_rect c = { f.x, f.y, f.w, f.h, 0.0f };
    r->dirty[r->n_dirty++] = c;
}
static int dirty_hits(struct scene_renderer *r, struct frect f) {
    if (r->full) return 1;
    for (int i = 0; i < r->n_dirty; i++) {
        struct frect d = { r->dirty[i].x, r->dirty[i].y, r->dirty[i].w, r->dirty[i].h };
        if (frect_overlap(f, d)) return 1;
    }
    return 0;
}

/* one gathered node (document order == paint order) */
struct noderec {
    struct node_handle h;
    struct frect       aabb;
    int                is_blur;
    int                has_group;
    struct frect       group_bounds;
    int                dirty;
};

static void gather(struct scene_arena *a, struct node_handle h, const float wparent[16],
                   int has_group, struct frect gbounds,
                   struct noderec *list, int *n, int cap) {
    struct scene_node *node = scene_resolve(a, h);
    if (!node || *n >= cap) return;
    float local[16], world[16];
    scene_trs_to_matrix(node, local);
    scene_mat4_mul(wparent, local, world);

    struct noderec *rec = &list[(*n)++];
    rec->h = h;
    rec->aabb = node_world_aabb(node, world);
    rec->is_blur = node->backdrop_blur_enabled;
    rec->has_group = has_group;
    rec->group_bounds = gbounds;
    rec->dirty = 0;

    int child_group = has_group;
    struct frect child_gb = gbounds;
    if (node->opacity < 1.0f) {      /* this node starts (or nests) a group */
        child_group = 1;
        child_gb = subtree_bounds(a, h, wparent);
    }
    struct node_handle c = node->first_child;
    uint32_t guard = 512;   /* cap sibling walks (cycle-safe) */
    while (!node_handle_is_null(c) && guard-- != 0) {
        struct scene_node *cn = scene_resolve(a, c);
        struct node_handle next = cn ? cn->next_sibling : NODE_HANDLE_NULL;
        gather(a, c, world, child_group, child_gb, list, n, cap);
        c = next;
    }
}

/* ------------------------------------------------------------------------- */
/* painting                                                                  */
/* ------------------------------------------------------------------------- */

static void paint_visual(struct scene_renderer *r, struct scene_node *n,
                         const float world[16], struct render_target *target,
                         float ox, float oy) {
    struct render_backend *be = r->be;
    struct frect ab = node_world_aabb(n, world);

    /* Node-level cull: if this node's painted footprint doesn't touch the dirty
     * region, its pixels are already correct on-target -- skip ALL per-pixel work
     * (text/rect/shadow). This is what makes an incremental frame cost scale with
     * the changed area, not the node count. dirty_hits() returns true under a
     * full repaint, so nothing is wrongly skipped then. The footprint is grown by
     * the shadow's reach so a shadow straddling the dirty edge still repaints. */
    float px0 = ab.x, py0 = ab.y, px1 = ab.x + ab.w, py1 = ab.y + ab.h;
    if (n->shadow_enabled) {
        float m = n->shadow_blur_radius + 2.0f;
        float sx0 = ab.x + n->shadow_dx - m,          sy0 = ab.y + n->shadow_dy - m;
        float sx1 = ab.x + ab.w + n->shadow_dx + m,   sy1 = ab.y + ab.h + n->shadow_dy + m;
        if (sx0 < px0) px0 = sx0;
        if (sy0 < py0) py0 = sy0;
        if (sx1 > px1) px1 = sx1;
        if (sy1 > py1) py1 = sy1;
    }
    struct frect foot = { px0 - ox, py0 - oy, px1 - px0, py1 - py0 };
    if (!dirty_hits(r, foot)) return;

    float dx = ab.x - ox, dy = ab.y - oy, w = ab.w, h = ab.h;

    switch (n->kind) {
        case SCENE_NODE_RECT:
            if (n->shadow_enabled)
                be->draw_shadow(target, dx, dy, w, h, n->corner_radius,
                                n->shadow_dx, n->shadow_dy, n->shadow_blur_radius, n->shadow_color);
            if (n->backdrop_blur_enabled)
                be->draw_backdrop_blur(target, dx, dy, w, h, n->corner_radius, n->backdrop_blur_radius);
            be->draw_rect(target, dx, dy, w, h, n->corner_radius, &n->data.rect.fill, 1.0f);
            if (n->border_width > 0 && be->draw_border)
                be->draw_border(target, dx, dy, w, h, n->corner_radius, n->border_width, n->border_color);
            break;
        case SCENE_NODE_IMAGE:
            be->draw_image(target, dx, dy, w, h, n->data.image.pixels,
                           n->data.image.w, n->data.image.h, n->data.image.w * 4,
                           n->data.image.fmt, 1.0f);
            break;
        case SCENE_NODE_TEXT:
            if (be->draw_text)
                be->draw_text(target, dx, dy, n->data.text.utf8, n->data.text.font_handle,
                              n->data.text.size_px, n->data.text.color, 1.0f);
            break;
        case SCENE_NODE_GROUP:
        default: break;
    }
}

static void render_node(struct scene_renderer *r, struct scene_arena *a, struct node_handle h,
                        const float wparent[16], struct render_target *target, float ox, float oy);
static void render_node_inner(struct scene_renderer *r, struct scene_arena *a, struct node_handle h,
                              const float wparent[16], struct render_target *target, float ox, float oy);

/* Depth-bounded wrapper: a corrupt scene tree with a parent<->child cycle would
 * otherwise recurse forever here (no fault, just a hang). Cap the depth well
 * above any real UI nesting so a cycle degrades instead of hanging. */
static int g_render_depth;
static void render_node(struct scene_renderer *r, struct scene_arena *a, struct node_handle h,
                        const float wparent[16], struct render_target *target, float ox, float oy) {
    if (g_render_depth > 1024) return;
    g_render_depth++;
    render_node_inner(r, a, h, wparent, target, ox, oy);
    g_render_depth--;
}

/* Paint a node's own visual (at opacity 1) + recurse children -- used for the
 * ROOT of a group's offscreen pass, where the group's own opacity is applied
 * once at the final blit, not here (Section 5 step 2). */
static void render_subtree_opaque(struct scene_renderer *r, struct scene_arena *a,
                                  struct node_handle h, const float wparent[16],
                                  struct render_target *target, float ox, float oy) {
    struct scene_node *n = scene_resolve(a, h);
    if (!n) return;
    float local[16], world[16];
    scene_trs_to_matrix(n, local);
    scene_mat4_mul(wparent, local, world);

    paint_visual(r, n, world, target, ox, oy);

    int pushed = 0;
    if (n->clip_children) {
        struct frect ab = node_world_aabb(n, world);
        struct clip_rect cr = { ab.x - ox, ab.y - oy, ab.w, ab.h, n->corner_radius };
        r->be->push_clip(target, cr); pushed = 1;
    }
    struct node_handle c = n->first_child;
    uint32_t guard = 512;   /* cap sibling walks (cycle-safe) */
    while (!node_handle_is_null(c) && guard-- != 0) {
        struct scene_node *cn = scene_resolve(a, c);
        struct node_handle next = cn ? cn->next_sibling : NODE_HANDLE_NULL;
        render_node(r, a, c, world, target, ox, oy);
        c = next;
    }
    if (pushed) r->be->pop_clip(target);
}

static void render_node_inner(struct scene_renderer *r, struct scene_arena *a, struct node_handle h,
                              const float wparent[16], struct render_target *target, float ox, float oy) {
    struct scene_node *n = scene_resolve(a, h);
    if (!n) return;
    float local[16], world[16];
    scene_trs_to_matrix(n, local);
    scene_mat4_mul(wparent, local, world);

    if (n->opacity < 1.0f) {
        /* Section 5: grouped offscreen compositing -- render the whole subtree
         * at full opacity into a scratch target, then blit once with the
         * group's opacity, so overlapping children blend against each other at
         * full strength (no seam) and only the flattened result is dimmed. */
        struct frect b = subtree_bounds(a, h, wparent);
        int bx = (int)floorf(b.x), by = (int)floorf(b.y);
        int bw = (int)ceilf(b.x + b.w) - bx, bh = (int)ceilf(b.y + b.h) - by;
        if (bw <= 0 || bh <= 0) return;
        if (!dirty_hits(r, b)) return;   /* whole group outside dirty -> skip */

        struct render_target *scratch = cpu_scratch_acquire((uint32_t)bw, (uint32_t)bh);
        if (scratch) {
            struct clip_saved sv;
            cpu_clip_save_and_clear(&sv);   /* isolate scratch coords + lift dirty */
            render_subtree_opaque(r, a, h, wparent, scratch, (float)bx, (float)by);
            cpu_clip_restore(&sv);
            /* blit flattened result; coverage_at clips this to the dirty union */
            r->be->draw_image(target, (float)bx - ox, (float)by - oy, (float)bw, (float)bh,
                              scratch->pixels, (uint32_t)bw, (uint32_t)bh, scratch->stride,
                              scratch->format, n->opacity);
            cpu_scratch_release(scratch);
        }
        return;
    }

    paint_visual(r, n, world, target, ox, oy);

    int pushed = 0;
    if (n->clip_children) {
        struct frect ab = node_world_aabb(n, world);
        struct clip_rect cr = { ab.x - ox, ab.y - oy, ab.w, ab.h, n->corner_radius };
        r->be->push_clip(target, cr); pushed = 1;
    }
    struct node_handle c = n->first_child;
    uint32_t guard = 512;   /* cap sibling walks (cycle-safe) */
    while (!node_handle_is_null(c) && guard-- != 0) {
        struct scene_node *cn = scene_resolve(a, c);
        struct node_handle next = cn ? cn->next_sibling : NODE_HANDLE_NULL;
        render_node(r, a, c, world, target, ox, oy);
        c = next;
    }
    if (pushed) r->be->pop_clip(target);
}

/* ------------------------------------------------------------------------- */
/* frame                                                                     */
/* ------------------------------------------------------------------------- */

void scene_render_init(struct scene_renderer *r, struct render_backend *be) {
    r->be = be; r->cache = 0; r->cache_cap = 0; r->n_dirty = 0; r->full = 0;
}
void scene_render_destroy(struct scene_renderer *r) {
    free(r->cache); r->cache = 0; r->cache_cap = 0;
}

static void ensure_cache(struct scene_renderer *r, uint32_t need) {
    if (r->cache_cap >= need) return;
    uint32_t cap = r->cache_cap ? r->cache_cap : 64;
    while (cap < need) cap *= 2;
    struct rect_cache *nc = (struct rect_cache *)realloc(r->cache, sizeof(*nc) * cap);
    if (!nc) return;
    for (uint32_t i = r->cache_cap; i < cap; i++) { nc[i].valid = 0; nc[i].generation = 0; }
    r->cache = nc; r->cache_cap = cap;
}

void scene_render_frame(struct scene_renderer *r, struct scene_arena *a,
                        struct node_handle root, struct render_target *target) {
    r->n_dirty = 0; r->full = 0;
    ensure_cache(r, a->next_never_used);
    if (!r->cache) return;

    /* gather every node once, in paint order, with world AABB + group context */
    uint32_t cap = a->next_never_used;
    struct noderec *recs = (struct noderec *)malloc(sizeof(struct noderec) * (cap ? cap : 1));
    if (!recs) return;
    int nrec = 0;
    float ident[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    gather(a, root, ident, 0, frect_empty(), recs, &nrec, (int)cap);

    /* --- Step 1: dirty accumulation (own dirty flag OR world rect moved OR
     * newly created); union BOTH the new rect and the cached old rect so a
     * moved element doesn't leave a ghost at its previous location. --- */
    for (int i = 0; i < nrec; i++) {
        struct noderec *rec = &recs[i];
        uint32_t idx = rec->h.index;
        struct scene_node *n = scene_resolve(a, rec->h);
        struct rect_cache *cc = (idx < r->cache_cap) ? &r->cache[idx] : 0;
        int moved = 1;
        if (cc && cc->valid && cc->generation == rec->h.generation) {
            moved = !(fabsf(cc->x - rec->aabb.x) < 0.01f && fabsf(cc->y - rec->aabb.y) < 0.01f &&
                      fabsf(cc->w - rec->aabb.w) < 0.01f && fabsf(cc->h - rec->aabb.h) < 0.01f);
        }
        int own_dirty = n && n->dirty;
        rec->dirty = own_dirty || moved;
        if (rec->dirty) {
            dirty_add(r, rec->aabb);                                  /* new footprint */
            if (cc && cc->valid && cc->generation == rec->h.generation) {
                struct frect old = { cc->x, cc->y, cc->w, cc->h };
                dirty_add(r, old);                                     /* old ghost */
            }
        }
    }

    /* --- Step 2: backdrop-blur footprint expansion. Any dirty rect touching a
     * blur node forces a full back-to-front repaint of that whole region, so
     * the blur samples FRESH behind-content, not last frame's pixels. --- */
    for (int i = 0; i < nrec; i++) {
        if (recs[i].is_blur && dirty_hits(r, recs[i].aabb))
            dirty_add(r, recs[i].aabb);
    }

    /* --- Step 3: opacity-group atomicity. A dirty rect intersecting any node
     * inside an opacity<1 group promotes to that group's whole bounds (groups
     * re-render as a unit; no partial-scratch update). --- */
    for (int i = 0; i < nrec; i++) {
        if (recs[i].has_group && dirty_hits(r, recs[i].aabb))
            dirty_add(r, recs[i].group_bounds);
    }

    /* --- Step 4: cap already collapses to full-screen in dirty_add. Paint the
     * whole tree back-to-front; coverage_at() (backend) restricts actual pixel
     * writes to the dirty union, and render_node skips groups outside it. --- */
    int have_work = r->full || r->n_dirty > 0;
    if (have_work) {
        if (r->full) {
            struct clip_rect fs = { 0, 0, (float)target->width, (float)target->height, 0 };
            r->be->begin_frame(target, &fs, 1);
        } else {
            r->be->begin_frame(target, r->dirty, (uint32_t)r->n_dirty);
        }
        render_node(r, a, root, ident, target, 0.0f, 0.0f);
        r->be->end_frame(target);
    }

    /* update cache to this frame's rects + clear consumed dirty flags */
    for (int i = 0; i < nrec; i++) {
        uint32_t idx = recs[i].h.index;
        if (idx < r->cache_cap) {
            r->cache[idx].x = recs[i].aabb.x; r->cache[idx].y = recs[i].aabb.y;
            r->cache[idx].w = recs[i].aabb.w; r->cache[idx].h = recs[i].aabb.h;
            r->cache[idx].generation = recs[i].h.generation;
            r->cache[idx].valid = 1;
        }
        struct scene_node *n = scene_resolve(a, recs[i].h);
        if (n) n->dirty = 0;
    }
    free(recs);
}
