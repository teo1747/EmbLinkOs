/* ui/backend/cpu_backend.c -- EmbLink UI Piece 4a: the CPU render backend.
 *
 * Primitives that draw into a premultiplied-BGRA8888 render_target: SDF
 * rounded-rect AA (Section 3), image blit, box-blur backdrop (Section 4),
 * shadow, a rounded-rect clip stack, and a growable scratch-buffer pool
 * (Section 5's opacity groups + Section 4's blur workspace). The dirty-rect
 * orchestration and opacity-group compositing live one level up in
 * scene_render.c -- these are the stateless-ish per-primitive operations it
 * calls.
 *
 * Host-buildable (uses libm sqrtf/fabsf); a freestanding ring-3 port needs
 * tiny sqrtf/fabsf shims -- flagged, not solved here (same host-first posture
 * as Piece 3's scene tree). */

#include "backend.h"
#include <stdlib.h>
#include <math.h>

/* ------------------------------------------------------------------------- */
/* premultiplied-BGRA8888 pixel helpers                                      */
/* ------------------------------------------------------------------------- */

static inline uint32_t px_pack(int b, int g, int r, int a) {
    return ((uint32_t)(a & 255) << 24) | ((uint32_t)(r & 255) << 16)
         | ((uint32_t)(g & 255) << 8)  | (uint32_t)(b & 255);
}
static inline void px_unpack(uint32_t p, int *b, int *g, int *r, int *a) {
    *b = (int)(p & 255); *g = (int)((p >> 8) & 255);
    *r = (int)((p >> 16) & 255); *a = (int)((p >> 24) & 255);
}
static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

static inline uint32_t *rt_row(struct render_target *rt, uint32_t y) {
    return (uint32_t *)((unsigned char *)rt->pixels + (size_t)y * rt->stride);
}

/* Blend a premultiplied source (channels & alpha already * pa, all 0..1) over
 * the destination pixel at (ix,iy): dst = src + dst*(1-pa). */
static void blend_over(struct render_target *rt, int ix, int iy,
                       float pr, float pg, float pb, float pa) {
    if (pa <= 0.0f) return;
    if (ix < 0 || iy < 0 || (uint32_t)ix >= rt->width || (uint32_t)iy >= rt->height) return;
    uint32_t *px = &rt_row(rt, (uint32_t)iy)[ix];
    int db, dg, dr, da; px_unpack(*px, &db, &dg, &dr, &da);
    float inv = 1.0f - pa;
    int nb = clampi((int)(pb * 255.0f + db * inv + 0.5f), 0, 255);
    int ng = clampi((int)(pg * 255.0f + dg * inv + 0.5f), 0, 255);
    int nr = clampi((int)(pr * 255.0f + dr * inv + 0.5f), 0, 255);
    int na = clampi((int)(pa * 255.0f + da * inv + 0.5f), 0, 255);
    *px = px_pack(nb, ng, nr, na);
}

/* ------------------------------------------------------------------------- */
/* rounded-box SDF (Section 3) -- Inigo Quilez, per-pixel on the CPU          */
/* ------------------------------------------------------------------------- */

static inline __attribute__((always_inline))
float rounded_box_sdf(float px, float py, float half_w, float half_h, float radius) {
    float qx = fabsf(px) - half_w + radius;
    float qy = fabsf(py) - half_h + radius;
    float mx = qx > 0 ? qx : 0, my = qy > 0 ? qy : 0;
    /* sqrt only in the rounded-corner quadrant (both axis distances > 0). Inside
     * and straight-edge pixels -- the overwhelming majority when filling a rect
     * or testing a dirty/clip rect -- have mx==0 or my==0, so skip the (in this
     * -mno-sse newlib build, libm-CALL) sqrt entirely. This is the dominant
     * per-pixel cost: rounded_box_sdf runs ~10x/pixel (fill + dirty + clips). */
    float outside;
    if      (mx == 0.0f) outside = my;
    else if (my == 0.0f) outside = mx;
    else                 outside = sqrtf(mx * mx + my * my);
    float inside = (qx > qy ? qx : qy); if (inside > 0) inside = 0;
    return outside + inside - radius;
}
/* ~1px smoothstep coverage straddling the boundary. */
static inline __attribute__((always_inline))
float sdf_coverage(float dist) { return clampf(0.5f - dist, 0.0f, 1.0f); }

/* ------------------------------------------------------------------------- */
/* clip stack (rounded-rect, intersecting) -- module state                   */
/* ------------------------------------------------------------------------- */

#define CLIP_MAX 32
static struct clip_rect g_clip[CLIP_MAX];
static int g_clip_n;

static void cpu_push_clip(struct render_target *rt, struct clip_rect r) {
    (void)rt;
    if (g_clip_n < CLIP_MAX) g_clip[g_clip_n] = r;
    g_clip_n++;   /* still count past the cap so pop stays balanced */
}
static void cpu_pop_clip(struct render_target *rt) { (void)rt; if (g_clip_n > 0) g_clip_n--; }

/* Per-frame dirty region: a UNION of rects (Section 6). Distinct from the clip
 * stack, which INTERSECTS. g_dirty_full == 1 means "no restriction" (used both
 * for a full-screen frame and while rendering into a scratch buffer, whose own
 * coordinate space the target-space dirty rects don't apply to). */
#define DIRTY_MAX 16
static struct clip_rect g_dirty[DIRTY_MAX];
static int g_dirty_n;
static int g_dirty_full = 1;

/* Product of every active clip's coverage at pixel-center (fx,fy). Hot path:
 * runs once per drawn pixel per primitive, so it fast-accepts the common case.
 * A pixel more than (radius + 0.5) inside every edge of a clip is fully covered
 * by it (the rounded corner only curves within `radius` of a corner, and the AA
 * band is <=0.5px) -- test that cheaply and skip the SDF+sqrt entirely; only
 * pixels within ~1px of a clip edge pay for the exact rounded_box_sdf. */
static float clip_stack_coverage(float fx, float fy) {
    int n = g_clip_n < CLIP_MAX ? g_clip_n : CLIP_MAX;
    float cov = 1.0f;
    for (int i = 0; i < n; i++) {
        struct clip_rect *c = &g_clip[i];
        float hw = c->w * 0.5f, hh = c->h * 0.5f;
        float dx = fx - (c->x + hw), dy = fy - (c->y + hh);
        float adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
        float inset = c->corner_radius + 0.5f;
        if (adx <= hw - inset && ady <= hh - inset) continue;   /* fully inside */
        float d = rounded_box_sdf(dx, dy, hw, hh, c->corner_radius);
        cov *= sdf_coverage(d);
        if (cov <= 0.0f) return 0.0f;
    }
    return cov;
}
/* Coverage over the dirty union. Damage rects are axis-aligned integer boxes --
 * a pixel is wholly in or wholly out -- so a plain containment test is both
 * cheaper (no rounded_box_sdf/sqrt per rect) and more correct (a rounded/AA'd
 * damage edge would fade the primitive out at an arbitrary tile seam). */
static float dirty_coverage(float fx, float fy) {
    if (g_dirty_full) return 1.0f;
    int n = g_dirty_n < DIRTY_MAX ? g_dirty_n : DIRTY_MAX;
    for (int i = 0; i < n; i++) {
        struct clip_rect *c = &g_dirty[i];
        if (fx >= c->x && fx < c->x + c->w && fy >= c->y && fy < c->y + c->h)
            return 1.0f;
    }
    return 0.0f;
}
/* The one coverage every draw primitive multiplies its own AA by: clip stack
 * (intersect) AND the dirty union. */
static inline __attribute__((always_inline))
float coverage_at(float fx, float fy) {
    float dc = dirty_coverage(fx, fy);
    if (dc <= 0.0f) return 0.0f;
    return clip_stack_coverage(fx, fy) * dc;
}

/* 1 iff coverage at (fx,fy) is EXACTLY 1.0 and provably so with integer-cheap
 * tests alone: inside a dirty rect (axis-aligned, all-or-nothing) and in the
 * fast-accept interior of every clip (> corner_radius+0.5 from each edge).
 * Anything else -- incl. pixels in a clip's AA band -- returns 0 and the
 * caller takes the exact slow path. This powers the solid-opaque fill fast
 * path: interior pixels skip the SDF + paint + float blend entirely. */
static inline __attribute__((always_inline))
int coverage_full_at(float fx, float fy) {
    if (!g_dirty_full) {
        int inside = 0;
        int n = g_dirty_n < DIRTY_MAX ? g_dirty_n : DIRTY_MAX;
        for (int i = 0; i < n; i++) {
            struct clip_rect *c = &g_dirty[i];
            if (fx >= c->x && fx < c->x + c->w && fy >= c->y && fy < c->y + c->h) { inside = 1; break; }
        }
        if (!inside) return 0;
    }
    int n = g_clip_n < CLIP_MAX ? g_clip_n : CLIP_MAX;
    for (int i = 0; i < n; i++) {
        struct clip_rect *c = &g_clip[i];
        float hw = c->w * 0.5f, hh = c->h * 0.5f;
        float dx = fx - (c->x + hw), dy = fy - (c->y + hh);
        float adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
        float inset = c->corner_radius + 0.5f;
        if (!(adx <= hw - inset && ady <= hh - inset)) return 0;
    }
    return 1;
}

/* Integer bounding box of the dirty union, clamped to the target. Primitives
 * intersect their own loop bounds with this so per-pixel cost scales with the
 * dirty area, not the node size -- otherwise a full-surface background (or a
 * blurred shadow) recomputes an entire pass every frame even when two pixels
 * changed. Returns 0 when the intersection is empty (nothing to draw). */
static int dirty_bounds(struct render_target *rt, int *bx0, int *by0, int *bx1, int *by1) {
    if (g_dirty_full) { *bx0 = 0; *by0 = 0; *bx1 = (int)rt->width; *by1 = (int)rt->height; return 1; }
    int n = g_dirty_n < DIRTY_MAX ? g_dirty_n : DIRTY_MAX;
    if (n <= 0) return 0;
    float minx = 1e30f, miny = 1e30f, maxx = -1e30f, maxy = -1e30f;
    for (int i = 0; i < n; i++) {
        struct clip_rect *c = &g_dirty[i];
        if (c->x < minx) minx = c->x;
        if (c->y < miny) miny = c->y;
        if (c->x + c->w > maxx) maxx = c->x + c->w;
        if (c->y + c->h > maxy) maxy = c->y + c->h;
    }
    *bx0 = clampi((int)floorf(minx), 0, (int)rt->width);
    *by0 = clampi((int)floorf(miny), 0, (int)rt->height);
    *bx1 = clampi((int)ceilf(maxx),  0, (int)rt->width);
    *by1 = clampi((int)ceilf(maxy),  0, (int)rt->height);
    return (*bx1 > *bx0 && *by1 > *by0);
}
/* Intersect [*x0,*x1)x[*y0,*y1) with the dirty bbox in place; 0 if it empties. */
static int clamp_to_dirty(struct render_target *rt, int *x0, int *y0, int *x1, int *y1) {
    int dx0, dy0, dx1, dy1;
    if (!dirty_bounds(rt, &dx0, &dy0, &dx1, &dy1)) return 0;
    if (*x0 < dx0) *x0 = dx0;
    if (*y0 < dy0) *y0 = dy0;
    if (*x1 > dx1) *x1 = dx1;
    if (*y1 > dy1) *y1 = dy1;
    return (*x1 > *x0 && *y1 > *y0);
}

/* Public accessor for the Piece-4b text blit (separate translation unit). */
float cpu_coverage_at(float fx, float fy) { return coverage_at(fx, fy); }

/* Install the frame's dirty rects (n==0 => full-screen, no restriction). */
void cpu_set_dirty(const struct clip_rect *rects, uint32_t n) {
    if (n == 0 || !rects) { g_dirty_full = 1; g_dirty_n = 0; return; }
    g_dirty_full = 0;
    g_dirty_n = (int)(n < DIRTY_MAX ? n : DIRTY_MAX);
    for (int i = 0; i < g_dirty_n; i++) g_dirty[i] = rects[i];
}

void cpu_clip_save_and_clear(struct clip_saved *s) {
    s->n = g_clip_n < CLIP_MAX ? g_clip_n : CLIP_MAX;
    for (int i = 0; i < s->n; i++) s->rects[i] = g_clip[i];
    g_clip_n = 0;
    /* a scratch render is unrestricted -- save & lift the dirty region too */
    s->dirty_n = g_dirty_n; s->dirty_full = g_dirty_full;
    for (int i = 0; i < s->dirty_n; i++) s->dirty_rects[i] = g_dirty[i];
    g_dirty_full = 1; g_dirty_n = 0;
}
void cpu_clip_restore(const struct clip_saved *s) {
    g_clip_n = s->n;
    for (int i = 0; i < s->n; i++) g_clip[i] = s->rects[i];
    g_dirty_full = s->dirty_full; g_dirty_n = s->dirty_n;
    for (int i = 0; i < s->dirty_n; i++) g_dirty[i] = s->dirty_rects[i];
}

/* ------------------------------------------------------------------------- */
/* growable scratch-buffer pool (Section 5)                                  */
/* ------------------------------------------------------------------------- */

#define SCRATCH_SLOTS 64
struct scratch_slot { struct render_target rt; uint32_t cap_px; int in_use; };
static struct scratch_slot g_scratch[SCRATCH_SLOTS];

struct render_target *cpu_scratch_acquire(uint32_t w, uint32_t h) {
    if (w == 0) w = 1;
    if (h == 0) h = 1;
    uint32_t need = w * h;
    /* prefer a free slot whose buffer is already big enough */
    struct scratch_slot *pick = 0;
    for (int i = 0; i < SCRATCH_SLOTS; i++) {
        if (!g_scratch[i].in_use && g_scratch[i].cap_px >= need) { pick = &g_scratch[i]; break; }
    }
    if (!pick) {
        for (int i = 0; i < SCRATCH_SLOTS; i++) {
            if (!g_scratch[i].in_use) { pick = &g_scratch[i]; break; }
        }
        if (!pick) return 0;   /* pool exhausted (64 simultaneously-held is very deep) */
        if (pick->cap_px < need) {
            void *p = realloc(pick->rt.pixels, (size_t)need * 4);
            if (!p) return 0;
            pick->rt.pixels = p;
            pick->cap_px = need;
        }
    }
    pick->in_use = 1;
    pick->rt.width = w; pick->rt.height = h; pick->rt.stride = w * 4;
    pick->rt.format = EMBK_PIXFMT_BGRA8888_PRE;
    /* zero -> fully transparent, so a group composites correctly */
    for (uint32_t i = 0; i < need; i++) ((uint32_t *)pick->rt.pixels)[i] = 0;
    return &pick->rt;
}

void cpu_scratch_release(struct render_target *rt) {
    for (int i = 0; i < SCRATCH_SLOTS; i++) {
        if (&g_scratch[i].rt == rt) { g_scratch[i].in_use = 0; return; }
    }
}

/* ------------------------------------------------------------------------- */
/* frame lifecycle                                                           */
/* ------------------------------------------------------------------------- */

static void cpu_begin_frame(struct render_target *rt, const struct clip_rect *dirty, uint32_t n) {
    (void)rt;
    g_clip_n = 0;             /* node clips are pushed per-node by the driver */
    cpu_set_dirty(dirty, n);  /* the frame's dirty union (Section 6) */
}
static void cpu_end_frame(struct render_target *rt) { (void)rt; }

/* ------------------------------------------------------------------------- */
/* paint evaluation                                                          */
/* ------------------------------------------------------------------------- */

static struct color lerp_color(struct color a, struct color b, float t) {
    struct color c;
    c.r = a.r + (b.r - a.r) * t; c.g = a.g + (b.g - a.g) * t;
    c.b = a.b + (b.b - a.b) * t; c.a = a.a + (b.a - a.a) * t;
    return c;
}
static struct color gradient_at(const struct paint *p, float t) {
    t = clampf(t, 0.0f, 1.0f);
    if (p->n_stops == 0) return p->solid;
    if (t <= p->stops[0].offset) return p->stops[0].color;
    for (int i = 1; i < p->n_stops; i++) {
        if (t <= p->stops[i].offset) {
            float span = p->stops[i].offset - p->stops[i-1].offset;
            float local = span > 1e-6f ? (t - p->stops[i-1].offset) / span : 0.0f;
            return lerp_color(p->stops[i-1].color, p->stops[i].color, local);
        }
    }
    return p->stops[p->n_stops - 1].color;
}
/* Straight-alpha paint color at rect-local (u,v) in [0,w]x[0,h]. */
static struct color paint_at(const struct paint *p, float u, float v, float w, float h) {
    switch (p->kind) {
        case PAINT_SOLID: return p->solid;
        case PAINT_LINEAR_GRADIENT: {
            float ang = p->angle_deg * 3.14159265f / 180.0f;
            float dx = cosf(ang), dy = sinf(ang);
            float denom = fabsf(dx) * w + fabsf(dy) * h;
            float t = denom > 1e-6f ? (u * dx + v * dy) / denom + 0.5f : 0.0f;
            return gradient_at(p, t);
        }
        case PAINT_RADIAL_GRADIENT: {
            float ddx = u - p->center_x, ddy = v - p->center_y;
            float d = sqrtf(ddx * ddx + ddy * ddy);
            return gradient_at(p, p->radius > 1e-6f ? d / p->radius : 0.0f);
        }
        default: { struct color none = {0,0,0,0}; return none; }
    }
}

/* ------------------------------------------------------------------------- */
/* draw_rect (SDF AA + clip + paint)                                         */
/* ------------------------------------------------------------------------- */

static void cpu_draw_rect(struct render_target *rt, float x, float y, float w, float h,
                          float radius, const struct paint *fill, float opacity) {
    if (!fill || w <= 0 || h <= 0 || opacity <= 0) return;
    float half_w = w * 0.5f, half_h = h * 0.5f;
    float cx = x + half_w, cy = y + half_h;
    if (radius > half_w) radius = half_w;
    if (radius > half_h) radius = half_h;

    int x0 = (int)floorf(x - 1), y0 = (int)floorf(y - 1);
    int x1 = (int)ceilf(x + w + 1), y1 = (int)ceilf(y + h + 1);
    x0 = clampi(x0, 0, (int)rt->width);  x1 = clampi(x1, 0, (int)rt->width);
    y0 = clampi(y0, 0, (int)rt->height); y1 = clampi(y1, 0, (int)rt->height);
    if (!clamp_to_dirty(rt, &x0, &y0, &x1, &y1)) return;

    /* SOLID-OPAQUE FAST PATH: for a solid fill at full alpha and opacity, every
     * pixel at least 1px inside the rect (and past the corner bands) with full
     * coverage is just a store of one precomputed packed value -- no SDF, no
     * paint_at struct return, no float unpack/blend/pack. This is what makes a
     * first full-window paint tolerable under TCG (float-per-pixel there cost
     * ~10s for one 560x728 frame). Edges/corners/AA bands keep the exact path. */
    int fast_solid = (fill->kind == PAINT_SOLID &&
                      fill->solid.a >= 0.999f && opacity >= 0.999f);
    /* CONSTANT-ALPHA (TRANSLUCENT) SOLID FAST PATH: a solid fill with a fixed
     * partial alpha -- the modal scrim (a=0.55 over the whole window), tinted
     * banners, accent-soft fills -- has NO fast path above, so every interior
     * pixel took the float blend_over. For a 560x760 scrim that's ~425k float
     * blends per frame; under TCG one modal frame took multiple SECONDS, and
     * screenshots caught it mid-render (a scrim covering only the top rows, no
     * dialog yet -- the "modal doesn't render" bug). The blend for a constant
     * premultiplied source over the interior is per-channel integer:
     *   out = round(dst*(1-a)) + round(src*a)
     * which is carry-safe (max is 255) and bakes into four 256-entry LUTs. */
    int fast_alpha = (fill->kind == PAINT_SOLID && !fast_solid && opacity >= 0.999f &&
                      fill->solid.a > 0.003f && fill->solid.a < 0.999f);
    uint32_t solid_pix = 0;
    uint8_t lutB[256], lutG[256], lutR[256], lutA[256];
    int in_x0 = 0, in_x1 = -1, in_y0 = 0, in_y1 = -1;
    if (fast_solid || fast_alpha) {
        if (fast_solid) {
            solid_pix = px_pack((int)(fill->solid.b * 255.0f + 0.5f),
                                (int)(fill->solid.g * 255.0f + 0.5f),
                                (int)(fill->solid.r * 255.0f + 0.5f), 255);
        } else {
            float a = fill->solid.a, inv = 1.0f - a;
            int sB = (int)(fill->solid.b * a * 255.0f + 0.5f);
            int sG = (int)(fill->solid.g * a * 255.0f + 0.5f);
            int sR = (int)(fill->solid.r * a * 255.0f + 0.5f);
            int sA = (int)(a * 255.0f + 0.5f);
            for (int v = 0; v < 256; v++) {
                int d = (int)(v * inv + 0.5f);
                lutB[v] = (uint8_t)(d + sB > 255 ? 255 : d + sB);
                lutG[v] = (uint8_t)(d + sG > 255 ? 255 : d + sG);
                lutR[v] = (uint8_t)(d + sR > 255 ? 255 : d + sR);
                lutA[v] = (uint8_t)(d + sA > 255 ? 255 : d + sA);
            }
        }
        /* rows clear of the top/bottom corner bands; columns 1px inside */
        in_y0 = (int)ceilf(y + radius + 1.0f);
        in_y1 = (int)floorf(y + h - radius - 1.0f);
        in_x0 = (int)ceilf(x + 1.0f);
        in_x1 = (int)floorf(x + w - 1.0f);
    }
    /* one interior pixel: opaque -> a bare store; translucent -> an integer
     * LUT blend over the existing dst. Both avoid the float blend_over. */
    #define CPU_INTERIOR_STORE(ROW, IX) do {                                   \
        if (fast_solid) (ROW)[IX] = solid_pix;                                 \
        else { int _b,_g,_r,_a; px_unpack((ROW)[IX], &_b,&_g,&_r,&_a);         \
               (ROW)[IX] = px_pack(lutB[_b], lutG[_g], lutR[_r], lutA[_a]); }  \
    } while (0)

    /* no dirty restriction and no clips at all -> interior coverage is 1.0 by
     * construction; the interior fill degenerates to a bare row of stores. */
    int cov_trivial = (g_dirty_full && g_clip_n == 0);

    for (int iy = y0; iy < y1; iy++) {
        float fy = iy + 0.5f;
        uint32_t *row = rt_row(rt, (uint32_t)iy);
        int row_interior = (fast_solid || fast_alpha) && iy >= in_y0 && iy < in_y1;
        int sp_a = in_x0 > x0 ? in_x0 : x0, sp_b = in_x1 < x1 ? in_x1 : x1;
        if (row_interior && cov_trivial && sp_a < sp_b) {
            int a = sp_a, b = sp_b;
            for (int ix = a; ix < b; ix++) CPU_INTERIOR_STORE(row, ix);
            /* left/right AA columns still take the exact path below */
            for (int ix = x0; ix < x1; ix++) {
                if (ix == a) { ix = b - 1; continue; }   /* skip the filled span */
                float fx = ix + 0.5f;
                float dist = rounded_box_sdf(fx - cx, fy - cy, half_w, half_h, radius);
                float cov = sdf_coverage(dist);
                if (cov <= 0.0f) continue;
                cov *= coverage_at(fx, fy);
                if (cov <= 0.0f) continue;
                struct color c = paint_at(fill, fx - x, fy - y, w, h);
                float eff = c.a * cov * opacity;
                if (eff <= 0.0f) continue;
                blend_over(rt, ix, iy, c.r * eff, c.g * eff, c.b * eff, eff);
            }
            continue;
        }
        for (int ix = x0; ix < x1; ix++) {
            float fx = ix + 0.5f;
            if (row_interior && ix >= in_x0 && ix < in_x1 && coverage_full_at(fx, fy)) {
                CPU_INTERIOR_STORE(row, ix);
                continue;
            }
            float dist = rounded_box_sdf(fx - cx, fy - cy, half_w, half_h, radius);
            float cov = sdf_coverage(dist);
            if (cov <= 0.0f) continue;
            cov *= coverage_at(fx, fy);
            if (cov <= 0.0f) continue;
            struct color c = paint_at(fill, fx - x, fy - y, w, h);
            float eff = c.a * cov * opacity;
            if (eff <= 0.0f) continue;
            blend_over(rt, ix, iy, c.r * eff, c.g * eff, c.b * eff, eff);
        }
    }
    #undef CPU_INTERIOR_STORE
}

/* ------------------------------------------------------------------------- */
/* draw_image (nearest sample, premultiplied source)                         */
/* ------------------------------------------------------------------------- */

static void cpu_draw_image(struct render_target *rt, float x, float y, float w, float h,
                           const void *pixels, uint32_t src_w, uint32_t src_h,
                           uint32_t src_stride, enum embk_pixfmt src_fmt, float opacity) {
    (void)src_fmt;
    if (!pixels || w <= 0 || h <= 0 || src_w == 0 || src_h == 0 || opacity <= 0) return;
    int x0 = clampi((int)floorf(x), 0, (int)rt->width);
    int y0 = clampi((int)floorf(y), 0, (int)rt->height);
    int x1 = clampi((int)ceilf(x + w), 0, (int)rt->width);
    int y1 = clampi((int)ceilf(y + h), 0, (int)rt->height);
    if (!clamp_to_dirty(rt, &x0, &y0, &x1, &y1)) return;

    /* INTEGER fast path: at full opacity with full coverage the premultiplied
     * source-over is pure integer -- opaque source is a bare copy, translucent
     * is dst*(255-sa)/255 + src per channel (same carry-safe trick the shadow
     * blit uses). Avoids 4 float divides + the float blend_over per pixel; the
     * exact float path still runs for opacity<1 and clip/dirty AA edges. */
    int img_trivial_all = (opacity >= 0.999f) && g_dirty_full && g_clip_n == 0;
    int img_opaque_op   = (opacity >= 0.999f);

    for (int iy = y0; iy < y1; iy++) {
        float fy = iy + 0.5f;
        float v = (fy - y) / h; if (v < 0) v = 0; if (v >= 1) v = 0.999999f;
        uint32_t sy = (uint32_t)(v * src_h);
        const uint32_t *srow = (const uint32_t *)((const unsigned char *)pixels + (size_t)sy * src_stride);
        uint32_t *drow = rt_row(rt, (uint32_t)iy);
        for (int ix = x0; ix < x1; ix++) {
            float fx = ix + 0.5f;
            float u = (fx - x) / w; if (u < 0) u = 0; if (u >= 1) u = 0.999999f;
            uint32_t sx = (uint32_t)(u * src_w);
            uint32_t s = srow[sx];
            if (img_trivial_all || (img_opaque_op && coverage_full_at(fx, fy))) {
                uint32_t sa = s >> 24;
                if (!sa) continue;
                if (sa == 255) { drow[ix] = s; continue; }   /* opaque: bare copy */
                uint32_t inv = 255u - sa, dv = drow[ix];
                uint32_t t = (dv & 0xFF00FFu) * inv + 0x800080u;
                t = ((t + ((t >> 8) & 0xFF00FFu)) >> 8) & 0xFF00FFu;
                uint32_t g = ((dv >> 8) & 0xFF00FFu) * inv + 0x800080u;
                g = ((g + ((g >> 8) & 0xFF00FFu)) >> 8) & 0xFF00FFu;
                drow[ix] = (t + (s & 0xFF00FFu)) | ((g + ((s >> 8) & 0xFF00FFu)) << 8);
                continue;
            }
            int sb, sg, sr, sa; px_unpack(s, &sb, &sg, &sr, &sa);
            float factor = opacity * coverage_at(fx, fy);
            if (factor <= 0.0f) continue;
            /* source is premultiplied; scaling all channels by factor keeps it so */
            blend_over(rt, ix, iy, sr / 255.0f * factor, sg / 255.0f * factor,
                       sb / 255.0f * factor, sa / 255.0f * factor);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* 3-pass separable box blur over a premultiplied uint32 region              */
/* ------------------------------------------------------------------------- */

static void box_blur_1d(uint32_t *dst, const uint32_t *src, int n, int stride, int radius) {
    if (radius < 1) { for (int i = 0; i < n; i++) dst[i * stride] = src[i * stride]; return; }
    int win = 2 * radius + 1;
    long sb = 0, sg = 0, sr = 0, sa = 0;
    /* prime the window with clamped-edge samples */
    for (int k = -radius; k <= radius; k++) {
        int idx = clampi(k, 0, n - 1);
        int b, g, r, a; px_unpack(src[idx * stride], &b, &g, &r, &a);
        sb += b; sg += g; sr += r; sa += a;
    }
    for (int i = 0; i < n; i++) {
        dst[i * stride] = px_pack((int)(sb / win), (int)(sg / win), (int)(sr / win), (int)(sa / win));
        int out_idx = clampi(i - radius, 0, n - 1);
        int in_idx  = clampi(i + radius + 1, 0, n - 1);
        int ob, og, orr, oa; px_unpack(src[out_idx * stride], &ob, &og, &orr, &oa);
        int ib, ig, ir, ia; px_unpack(src[in_idx * stride], &ib, &ig, &ir, &ia);
        sb += ib - ob; sg += ig - og; sr += ir - orr; sa += ia - oa;
    }
}

/* Single-channel u8 box blur (shadow tiles are grayscale coverage -- blurring
 * a packed 4-channel copy of the same value did 4x the work plus a pack and
 * unpack per sample). Rolling-sum sliding window, same shape as box_blur_1d. */
static void box_blur_1d_u8(uint8_t *dst, const uint8_t *src, int n, int stride, int radius) {
    if (radius < 1) { for (int i = 0; i < n; i++) dst[i * stride] = src[i * stride]; return; }
    int win = 2 * radius + 1;
    /* divide by the (variable) window with a reciprocal multiply -- an integer
     * DIV per sample was the blur's dominant cost under TCG (helper call).
     * sum <= 255*win, so sum*recip < 255*65536 + slack: fits 32 bits. */
    uint32_t recip = (65536u + (uint32_t)win / 2) / (uint32_t)win;
    int sum = 0;
    for (int k = -radius; k <= radius; k++) sum += src[clampi(k, 0, n - 1) * stride];
    for (int i = 0; i < n; i++) {
        dst[i * stride] = (uint8_t)(((uint32_t)sum * recip) >> 16);
        sum += src[clampi(i + radius + 1, 0, n - 1) * stride]
             - src[clampi(i - radius,     0, n - 1) * stride];
    }
}
static void box_blur_u8(uint8_t *buf, int w, int h, int radius) {
    if (radius < 1 || w <= 0 || h <= 0) return;
    uint8_t *tmp = (uint8_t *)malloc((size_t)w * h);
    if (!tmp) return;
    for (int pass = 0; pass < 3; pass++) {          /* 3 passes ~= Gaussian */
        for (int y = 0; y < h; y++)                  /* horizontal */
            box_blur_1d_u8(&tmp[y * w], &buf[y * w], w, 1, radius);
        for (int x = 0; x < w; x++)                  /* vertical */
            box_blur_1d_u8(&buf[x], &tmp[x], h, w, radius);
    }
    free(tmp);
}

static void box_blur_region(uint32_t *buf, int w, int h, int radius) {
    if (radius < 1 || w <= 0 || h <= 0) return;
    uint32_t *tmp = (uint32_t *)malloc((size_t)w * h * 4);
    if (!tmp) return;
    for (int pass = 0; pass < 3; pass++) {          /* 3 passes ~= Gaussian */
        for (int y = 0; y < h; y++)                  /* horizontal */
            box_blur_1d(&tmp[y * w], &buf[y * w], w, 1, radius);
        for (int x = 0; x < w; x++)                  /* vertical */
            box_blur_1d(&buf[x], &tmp[x], h, w, radius);
    }
    free(tmp);
}

/* ------------------------------------------------------------------------- */
/* draw_backdrop_blur (Section 4): sample rt, blur, SDF-clipped write-back    */
/* ------------------------------------------------------------------------- */

static void cpu_draw_backdrop_blur(struct render_target *rt, float x, float y, float w, float h,
                                   float radius, float blur_radius) {
    if (w <= 0 || h <= 0) return;
    int x0 = clampi((int)floorf(x), 0, (int)rt->width);
    int y0 = clampi((int)floorf(y), 0, (int)rt->height);
    int x1 = clampi((int)ceilf(x + w), 0, (int)rt->width);
    int y1 = clampi((int)ceilf(y + h), 0, (int)rt->height);
    int rw = x1 - x0, rh = y1 - y0;
    if (rw <= 0 || rh <= 0) return;

    uint32_t *tile = (uint32_t *)malloc((size_t)rw * rh * 4);
    if (!tile) return;
    for (int j = 0; j < rh; j++)
        for (int i = 0; i < rw; i++)
            tile[j * rw + i] = rt_row(rt, (uint32_t)(y0 + j))[x0 + i];   /* everything BEHIND is already here */

    box_blur_region(tile, rw, rh, (int)(blur_radius + 0.5f));

    float half_w = w * 0.5f, half_h = h * 0.5f, cx = x + half_w, cy = y + half_h;
    if (radius > half_w) radius = half_w;
    if (radius > half_h) radius = half_h;
    for (int j = 0; j < rh; j++) {
        int iy = y0 + j; float fy = iy + 0.5f;
        for (int i = 0; i < rw; i++) {
            int ix = x0 + i; float fx = ix + 0.5f;
            float cov = sdf_coverage(rounded_box_sdf(fx - cx, fy - cy, half_w, half_h, radius));
            cov *= coverage_at(fx, fy);
            if (cov <= 0.0f) continue;
            int bb, bg, br, ba; px_unpack(tile[j * rw + i], &bb, &bg, &br, &ba);
            uint32_t *dp = &rt_row(rt, (uint32_t)iy)[ix];
            int db, dg, dr, da; px_unpack(*dp, &db, &dg, &dr, &da);
            /* lerp original -> blurred by coverage (rounded corners keep original) */
            int nb = (int)(bb * cov + db * (1 - cov) + 0.5f);
            int ng = (int)(bg * cov + dg * (1 - cov) + 0.5f);
            int nr = (int)(br * cov + dr * (1 - cov) + 0.5f);
            int na = (int)(ba * cov + da * (1 - cov) + 0.5f);
            *dp = px_pack(nb, ng, nr, na);
        }
    }
    free(tile);
}

/* ------------------------------------------------------------------------- */
/* draw_border: SDF ring stroke along the inside of the rounded edge          */
/* ------------------------------------------------------------------------- */

static void cpu_draw_border(struct render_target *rt, float x, float y, float w, float h,
                            float radius, float width, struct color color) {
    if (w <= 0 || h <= 0 || width <= 0 || color.a <= 0) return;
    float half_w = w * 0.5f, half_h = h * 0.5f, cx = x + half_w, cy = y + half_h;
    if (radius > half_w) radius = half_w;
    if (radius > half_h) radius = half_h;

    int x0 = clampi((int)floorf(x - 1), 0, (int)rt->width);
    int y0 = clampi((int)floorf(y - 1), 0, (int)rt->height);
    int x1 = clampi((int)ceilf(x + w + 1), 0, (int)rt->width);
    int y1 = clampi((int)ceilf(y + h + 1), 0, (int)rt->height);
    if (!clamp_to_dirty(rt, &x0, &y0, &x1, &y1)) return;

    /* The ring only lives within (radius + width + AA) of the outer edge; for
     * rows in the vertical middle band, skip the interior columns entirely.
     * Without this every bordered widget paid FULL-AREA float SDF for a 1px
     * ring (the 500x620 card: 310k evaluations for a ~2k-pixel border). */
    float band = radius + width + 1.5f;
    int mid_y0 = (int)ceilf(y + band), mid_y1 = (int)floorf(y + h - band);
    int lx1 = (int)ceilf(x + band);        /* end of the left edge span   */
    int rx0 = (int)floorf(x + w - band);   /* start of the right edge span */

    for (int iy = y0; iy < y1; iy++) {
        float fy = iy + 0.5f;
        int sp[2][2]; int nsp;
        if (iy >= mid_y0 && iy < mid_y1 && lx1 < rx0) {
            sp[0][0] = x0; sp[0][1] = lx1 < x1 ? lx1 : x1;
            sp[1][0] = rx0 > x0 ? rx0 : x0; sp[1][1] = x1;
            nsp = 2;
        } else {
            sp[0][0] = x0; sp[0][1] = x1; nsp = 1;
        }
        for (int s = 0; s < nsp; s++) {
            for (int ix = sp[s][0]; ix < sp[s][1]; ix++) {
                float fx = ix + 0.5f;
                float d = rounded_box_sdf(fx - cx, fy - cy, half_w, half_h, radius);
                /* ring = inside the outer edge (d<0) minus inside the inner edge
                 * (d < -width): coverage of the shell of thickness `width`. */
                float ring = sdf_coverage(d) - sdf_coverage(d + width);
                if (ring <= 0.0f) continue;
                ring *= coverage_at(fx, fy);
                if (ring <= 0.0f) continue;
                float eff = color.a * ring;
                blend_over(rt, ix, iy, color.r * eff, color.g * eff, color.b * eff, eff);
            }
        }
    }
}

/* ------------------------------------------------------------------------- */
/* draw_shadow: blurred rounded-rect coverage, tinted (not in T1-T5, but real)*/
/* ------------------------------------------------------------------------- */

/* ---- cached blurred-shadow coverage tiles -------------------------------- *
 * A drop shadow's coverage depends only on (w, h, corner, blur) -- not on the
 * card's content or screen position -- so re-running the (expensive) SDF-fill +
 * box-blur every frame is pure waste when an animating child forces the card to
 * repaint. Compute the FULL blurred coverage once per shape, cache it, and each
 * frame just composite the dirty sub-region out of the cache. This is the single
 * biggest interactive-render win for animated shadowed cards. */
#define SHADOW_CACHE_N 4
struct shadow_tile { int used, kw, kh, kr, kb, fw, fh; uint8_t *cov; unsigned lru; };
static struct shadow_tile g_shcache[SHADOW_CACHE_N];
static unsigned g_shclock;

static struct shadow_tile *shadow_tile_get(int iw, int ih, int ir, int br) {
    for (int i = 0; i < SHADOW_CACHE_N; i++) {
        struct shadow_tile *e = &g_shcache[i];
        if (e->used && e->kw == iw && e->kh == ih && e->kr == ir && e->kb == br) {
            e->lru = ++g_shclock; return e;
        }
    }
    struct shadow_tile *e = &g_shcache[0];   /* miss -> free or LRU slot */
    for (int i = 0; i < SHADOW_CACHE_N; i++) {
        if (!g_shcache[i].used) { e = &g_shcache[i]; break; }
        if (g_shcache[i].lru < e->lru) e = &g_shcache[i];
    }
    int fw = iw + 2 * br + 2, fh = ih + 2 * br + 2;
    if (fw <= 0 || fh <= 0) return 0;
    /* Grayscale coverage field: zero outside the rect (calloc), 255 in the
     * interior, exact SDF only in the ~2px edge/corner band. The old version
     * evaluated the float SDF for EVERY tile pixel and blurred a packed
     * 4-channel copy -- most of a first frame's shadow cost for nothing. */
    uint8_t *cov = (uint8_t *)calloc((size_t)fw * fh, 1);
    if (!cov) return 0;
    float half_w = iw * 0.5f, half_h = ih * 0.5f;
    float cx = (br + 1) + half_w, cy = (br + 1) + half_h;
    float rr = (float)ir; if (rr > half_w) rr = half_w; if (rr > half_h) rr = half_h;
    int ox0 = clampi((int)floorf(cx - half_w - 1), 0, fw), ox1 = clampi((int)ceilf(cx + half_w + 1), 0, fw);
    int oy0 = clampi((int)floorf(cy - half_h - 1), 0, fh), oy1 = clampi((int)ceilf(cy + half_h + 1), 0, fh);
    float band = rr + 1.5f;   /* corners + AA edge live within this inset */
    int ix0 = (int)ceilf(cx - half_w + band), ix1 = (int)floorf(cx + half_w - band);
    int iy0 = (int)ceilf(cy - half_h + band), iy1 = (int)floorf(cy + half_h - band);
    for (int j = oy0; j < oy1; j++) {
        uint8_t *row = &cov[(size_t)j * fw];
        int mid_row = (j >= iy0 && j < iy1 && ix0 < ix1);
        for (int i = ox0; i < ox1; i++) {
            if (mid_row && i >= ix0 && i < ix1) { row[i] = 255; continue; }
            float c = sdf_coverage(rounded_box_sdf(i + 0.5f - cx, j + 0.5f - cy, half_w, half_h, rr));
            row[i] = (uint8_t)clampi((int)(c * 255.0f + 0.5f), 0, 255);
        }
    }
    box_blur_u8(cov, fw, fh, br);
    if (e->cov) free(e->cov);
    e->used = 1; e->kw = iw; e->kh = ih; e->kr = ir; e->kb = br;
    e->fw = fw; e->fh = fh; e->cov = cov; e->lru = ++g_shclock;
    return e;
}

static void cpu_draw_shadow(struct render_target *rt, float x, float y, float w, float h,
                            float radius, float dx, float dy, float blur_radius,
                            struct color color) {
    if (w <= 0 || h <= 0) return;
    int br = (int)(blur_radius + 0.5f);
    float sx = x + dx, sy = y + dy;
    float half_w = w * 0.5f, half_h = h * 0.5f;
    if (radius > half_w) radius = half_w;
    if (radius > half_h) radius = half_h;

    /* shadow footprint intersected with the dirty region -> the pixels to touch */
    int x0 = clampi((int)floorf(sx - blur_radius - 1), 0, (int)rt->width);
    int y0 = clampi((int)floorf(sy - blur_radius - 1), 0, (int)rt->height);
    int x1 = clampi((int)ceilf(sx + w + blur_radius + 1), 0, (int)rt->width);
    int y1 = clampi((int)ceilf(sy + h + blur_radius + 1), 0, (int)rt->height);
    int dbx0, dby0, dbx1, dby1;
    if (!dirty_bounds(rt, &dbx0, &dby0, &dbx1, &dby1)) return;
    if (x0 < dbx0) x0 = dbx0;
    if (y0 < dby0) y0 = dby0;
    if (x1 > dbx1) x1 = dbx1;
    if (y1 > dby1) y1 = dby1;
    if (x1 <= x0 || y1 <= y0) return;   /* shadow doesn't touch the dirty area */

    struct shadow_tile *e = shadow_tile_get((int)(w + 0.5f), (int)(h + 0.5f), (int)(radius + 0.5f), br);
    if (!e) return;
    /* the cached tile's local (br+1,br+1) == the shadow rect top-left (sx,sy) */
    int lox = (int)floorf(sx) - br - 1, loy = (int)floorf(sy) - br - 1;

    /* INTEGER blit fast path: the shadow is one constant color, so at full
     * coverage the blend is dst*(255-ea) + src*ea per channel -- pure integer.
     * Two 256-entry packed LUTs (built per call; calls are 1-3 per frame) turn
     * each pixel into ~a dozen integer ops instead of ~25 TCG-amplified float
     * ops (this blit was the single biggest first-frame term). Lane math is
     * carry-safe: dst lane <= inv and src lane <= ea, inv + ea == 255. */
    int a255 = (int)(color.a * 255.0f + 0.5f);
    int r255 = (int)(color.r * 255.0f + 0.5f);
    int g255 = (int)(color.g * 255.0f + 0.5f);
    int b255 = (int)(color.b * 255.0f + 0.5f);
    uint32_t lut_br[256], lut_ga[256];   /* premultiplied src, {r|b} and {a|g} */
    for (int ea = 0; ea < 256; ea++) {
        lut_br[ea] = (uint32_t)(((r255 * ea / 255) << 16) | (b255 * ea / 255));
        lut_ga[ea] = (uint32_t)((ea << 16) | (g255 * ea / 255));
    }
    int sh_cov_trivial = (g_dirty_full && g_clip_n == 0);   /* coverage==1 everywhere */

    for (int iy = y0; iy < y1; iy++) {
        int ly = iy - loy; if (ly < 0 || ly >= e->fh) continue;
        const uint8_t *row = e->cov + (size_t)ly * e->fw;
        uint32_t *rrow = rt_row(rt, (uint32_t)iy);
        float fy = iy + 0.5f;
        for (int ix = x0; ix < x1; ix++) {
            int lx = ix - lox; if (lx < 0 || lx >= e->fw) continue;
            int a = row[lx];
            if (!a) continue;
            int ea = (a255 * a + 127) / 255;
            if (!ea) continue;
            if (sh_cov_trivial || coverage_full_at(ix + 0.5f, fy)) {
                uint32_t v = rrow[ix], inv = 255u - (uint32_t)ea;
                uint32_t t = (v & 0xFF00FFu) * inv + 0x800080u;
                t = ((t + ((t >> 8) & 0xFF00FFu)) >> 8) & 0xFF00FFu;
                uint32_t u = ((v >> 8) & 0xFF00FFu) * inv + 0x800080u;
                u = ((u + ((u >> 8) & 0xFF00FFu)) >> 8) & 0xFF00FFu;
                rrow[ix] = (t + lut_br[ea]) | ((u + lut_ga[ea]) << 8);
                continue;
            }
            float eff = color.a * (a / 255.0f) * coverage_at(ix + 0.5f, fy);
            if (eff <= 0.0f) continue;
            blend_over(rt, ix, iy, color.r * eff, color.g * eff, color.b * eff, eff);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* the singleton vtable                                                      */
/* ------------------------------------------------------------------------- */

static struct render_backend g_cpu_backend = {
    .begin_frame        = cpu_begin_frame,
    .end_frame          = cpu_end_frame,
    .push_clip          = cpu_push_clip,
    .pop_clip           = cpu_pop_clip,
    .draw_rect          = cpu_draw_rect,
    .draw_image         = cpu_draw_image,
    .draw_shadow        = cpu_draw_shadow,
    .draw_backdrop_blur = cpu_draw_backdrop_blur,
    .draw_border        = cpu_draw_border,
    .draw_text          = 0,   /* Piece 4b */
};

struct render_backend *cpu_backend_get(void) { return &g_cpu_backend; }
