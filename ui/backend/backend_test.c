/* ui/backend/backend_test.c -- EmbLink UI Piece 4a selftests (Section 8).
 *
 * Pure userland: operates on in-memory render_target buffers and compares
 * pixels directly. Host-compiled + run natively:  make backend-test
 * Exits 0 iff every T1..T5 invariant holds. */

#include "scene_render.h"
#include "backend.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  FAIL: %s\n", (msg)); g_fail++; } \
    else         { printf("  ok:   %s\n", (msg)); } \
} while (0)

/* ---- pixel helpers (BGRA8888 premultiplied) ---------------------------- */
static uint32_t bgra(int b, int g, int r, int a) {
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}
static void unbgra(uint32_t p, int *b, int *g, int *r, int *a) {
    *b = p & 255; *g = (p >> 8) & 255; *r = (p >> 16) & 255; *a = (p >> 24) & 255;
}
static struct render_target make_target(int w, int h) {
    struct render_target rt;
    rt.pixels = calloc((size_t)w * h, 4);
    rt.width = w; rt.height = h; rt.stride = w * 4;
    rt.format = EMBK_PIXFMT_BGRA8888_PRE;
    return rt;
}
static uint32_t px_at(struct render_target *rt, int x, int y) {
    return ((uint32_t *)((unsigned char *)rt->pixels + (size_t)y * rt->stride))[x];
}
static void fill_target(struct render_target *rt, uint32_t color) {
    for (uint32_t y = 0; y < rt->height; y++)
        for (uint32_t x = 0; x < rt->width; x++)
            ((uint32_t *)((unsigned char *)rt->pixels + (size_t)y * rt->stride))[x] = color;
}
static int alpha_at(struct render_target *rt, int x, int y) {
    int b,g,r,a; unbgra(px_at(rt,x,y), &b,&g,&r,&a); return a;
}
static int green_at(struct render_target *rt, int x, int y) {
    int b,g,r,a; unbgra(px_at(rt,x,y), &b,&g,&r,&a); return g;
}
static int red_at(struct render_target *rt, int x, int y) {
    int b,g,r,a; unbgra(px_at(rt,x,y), &b,&g,&r,&a); return r;
}
static int blue_at(struct render_target *rt, int x, int y) {
    int b,g,r,a; unbgra(px_at(rt,x,y), &b,&g,&r,&a); return b;
}

/* ---- T1: rounded-rect AA is live (SDF + smoothstep) -------------------- */
static void t1_aa(void) {
    printf("T1 rounded-rect antialiasing:\n");
    struct render_backend *be = cpu_backend_get();
    struct render_target rt = make_target(64, 64);

    struct paint fill; fill.kind = PAINT_SOLID; fill.solid = (struct color){1,0,0,1}; fill.n_stops = 0;
    be->begin_frame(&rt, NULL, 0);                        /* full-screen (no dirty restriction) */
    be->draw_rect(&rt, 8, 8, 48, 48, 8.0f, &fill, 1.0f);  /* radius-8 rounded rect */
    be->end_frame(&rt);

    CHECK(alpha_at(&rt, 32, 32) >= 250, "center is fully opaque");
    /* the bounding-box corner (8,8) is mathematically outside the rounded arc */
    CHECK(alpha_at(&rt, 8, 8) <= 5, "rounded corner is transparent");
    /* the rounded-corner arc crosses pixel centers at partial coverage (the
     * straight edges are axis-aligned at integer coords, so their AA band
     * falls exactly between pixel rows -- the arc is where smoothstep shows). */
    int found_partial = 0;
    for (int y = 8; y < 17 && !found_partial; y++)
        for (int x = 8; x < 17; x++) {
            int a = alpha_at(&rt, x, y);
            if (a > 20 && a < 235) { found_partial = 1; break; }
        }
    CHECK(found_partial, "a corner-arc pixel has partial (AA) coverage");

    free(rt.pixels);
}

/* ---- T2: backdrop blur samples FRESH behind-content -------------------- */
static void t2_backdrop(void) {
    printf("T2 backdrop blur samples the real content:\n");
    struct render_backend *be = cpu_backend_get();
    struct render_target rt = make_target(64, 64);

    /* checkerboard of opaque black/white (average luminance ~127) */
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            int on = ((x >> 2) + (y >> 2)) & 1;
            ((uint32_t *)((unsigned char *)rt.pixels + (size_t)y * rt.stride))[x] =
                on ? bgra(255,255,255,255) : bgra(0,0,0,255);
        }

    be->begin_frame(&rt, NULL, 0);
    be->draw_backdrop_blur(&rt, 16, 16, 32, 32, 0.0f, 5.0f);
    be->end_frame(&rt);

    long sum = 0; int n = 0;
    for (int y = 20; y < 44; y++) for (int x = 20; x < 44; x++) { sum += red_at(&rt, x, y); n++; }
    int avg = (int)(sum / n);
    /* a real sample of the checkerboard averages toward mid-grey; black
     * scratch memory or an unsampled region would be ~0 */
    CHECK(avg > 80 && avg < 175, "blurred region averages toward the checkerboard mean");

    free(rt.pixels);
}

/* build: two overlapping red rects; `grouped` wraps them in a 50%-opacity
 * group, else each rect is individually 50% opaque. Returns overlap+solo px. */
static void build_two_rects(int grouped, int *overlap_g, int *solo_g) {
    struct scene_arena a; scene_arena_init(&a);
    struct scene_renderer r; scene_render_init(&r, cpu_backend_get());
    struct render_target rt = make_target(64, 64);
    fill_target(&rt, bgra(255,255,255,255));   /* opaque white background */

    struct paint red; red.kind = PAINT_SOLID; red.solid = (struct color){1,0,0,1}; red.n_stops = 0;

    struct node_handle root = scene_create_node(&a, SCENE_NODE_GROUP, NODE_HANDLE_NULL);
    struct node_handle parent = root;
    if (grouped) {
        parent = scene_create_node(&a, SCENE_NODE_GROUP, root);
        scene_set_opacity(&a, parent, 0.5f);
    }
    struct node_handle A = scene_create_node(&a, SCENE_NODE_RECT, parent);
    struct node_handle B = scene_create_node(&a, SCENE_NODE_RECT, parent);
    scene_set_size(&a, A, 30, 30); scene_set_transform(&a, A, 10,10,0, 0,0,0,1, 1,1,1);
    scene_set_size(&a, B, 30, 30); scene_set_transform(&a, B, 25,25,0, 0,0,0,1, 1,1,1);
    scene_set_paint(&a, A, &red); scene_set_paint(&a, B, &red);
    if (!grouped) { scene_set_opacity(&a, A, 0.5f); scene_set_opacity(&a, B, 0.5f); }

    scene_render_frame(&r, &a, root, &rt);
    *overlap_g = green_at(&rt, 30, 30);   /* inside both rects */
    *solo_g    = green_at(&rt, 14, 14);   /* inside A only */

    free(rt.pixels); scene_render_destroy(&r); scene_arena_destroy(&a);
}

/* ---- T3: opacity group eliminates the overlap seam --------------------- */
static void t3_opacity_group(void) {
    printf("T3 opacity group removes the overlap seam:\n");
    int g_overlap, g_solo, u_overlap, u_solo;
    build_two_rects(1, &g_overlap, &g_solo);   /* grouped */
    build_two_rects(0, &u_overlap, &u_solo);   /* ungrouped (per-rect alpha) */

    /* grouped: overlap blends as ONE flattened red at 50% -> ~= solo region */
    CHECK(abs(g_overlap - g_solo) < 20, "grouped: overlap matches non-overlap (no seam)");
    /* ungrouped: the overlap double-blends -> measurably darker (lower green) */
    CHECK(u_overlap < g_overlap - 25, "ungrouped overlap is measurably darker (seam present)");
    printf("    grouped overlap g=%d solo g=%d | ungrouped overlap g=%d solo g=%d\n",
           g_overlap, g_solo, u_overlap, u_solo);
}

/* ---- T4: dirty-rect + backdrop-blur -> fresh sample on frame 2 --------- */
static void t4_dirty_blur(void) {
    printf("T4 dirty-rect forces a fresh backdrop sample:\n");
    struct scene_arena a; scene_arena_init(&a);
    struct scene_renderer r; scene_render_init(&r, cpu_backend_get());
    struct render_target rt = make_target(64, 64);
    fill_target(&rt, bgra(0,0,0,255));

    struct node_handle root = scene_create_node(&a, SCENE_NODE_GROUP, NODE_HANDLE_NULL);
    /* background full-target rect so dirty regions restore to a known colour */
    struct node_handle bg = scene_create_node(&a, SCENE_NODE_RECT, root);
    scene_set_size(&a, bg, 64, 64);
    struct paint bgp; bgp.kind = PAINT_SOLID; bgp.solid = (struct color){0,0,0,1}; bgp.n_stops = 0;
    scene_set_paint(&a, bg, &bgp);
    /* A: the changing element, behind B */
    struct node_handle A = scene_create_node(&a, SCENE_NODE_RECT, root);
    scene_set_size(&a, A, 24, 24); scene_set_transform(&a, A, 20,20,0, 0,0,0,1, 1,1,1);
    struct paint redp; redp.kind = PAINT_SOLID; redp.solid = (struct color){1,0,0,1}; redp.n_stops = 0;
    scene_set_paint(&a, A, &redp);
    /* B: a backdrop-blur panel over A, itself never changing */
    struct node_handle B = scene_create_node(&a, SCENE_NODE_RECT, root);
    scene_set_size(&a, B, 32, 32); scene_set_transform(&a, B, 16,16,0, 0,0,0,1, 1,1,1);
    struct paint clearp; clearp.kind = PAINT_SOLID; clearp.solid = (struct color){0,0,0,0}; clearp.n_stops = 0;
    scene_set_paint(&a, B, &clearp);            /* transparent fill; blur does the visual */
    scene_set_backdrop_blur(&a, B, 1, 4.0f);

    scene_render_frame(&r, &a, root, &rt);       /* frame 1: A is red */
    int red_f1 = red_at(&rt, 28, 28);
    CHECK(red_f1 > 60, "frame 1 blur region reflects red A");

    /* frame 2: A becomes blue (only A changes) */
    struct paint bluep; bluep.kind = PAINT_SOLID; bluep.solid = (struct color){0,0,1,1}; bluep.n_stops = 0;
    scene_set_paint(&a, A, &bluep);
    scene_render_frame(&r, &a, root, &rt);
    int red_f2 = red_at(&rt, 28, 28), blue_f2 = blue_at(&rt, 28, 28);
    CHECK(blue_f2 > 60 && blue_f2 > red_f2, "frame 2 blur region reflects NEW (blue) A, not stale red");

    free(rt.pixels); scene_render_destroy(&r); scene_arena_destroy(&a);
}

/* ---- T5: moved element leaves no ghost (union old+new) ----------------- */
static void t5_ghost(void) {
    printf("T5 moved element leaves no ghost:\n");
    struct scene_arena a; scene_arena_init(&a);
    struct scene_renderer r; scene_render_init(&r, cpu_backend_get());
    struct render_target rt = make_target(64, 64);

    struct node_handle root = scene_create_node(&a, SCENE_NODE_GROUP, NODE_HANDLE_NULL);
    struct node_handle bg = scene_create_node(&a, SCENE_NODE_RECT, root);
    scene_set_size(&a, bg, 64, 64);
    struct paint grey; grey.kind = PAINT_SOLID; grey.solid = (struct color){0.5f,0.5f,0.5f,1}; grey.n_stops = 0;
    scene_set_paint(&a, bg, &grey);
    struct node_handle rect = scene_create_node(&a, SCENE_NODE_RECT, root);
    scene_set_size(&a, rect, 16, 16); scene_set_transform(&a, rect, 4,4,0, 0,0,0,1, 1,1,1);
    struct paint redp; redp.kind = PAINT_SOLID; redp.solid = (struct color){1,0,0,1}; redp.n_stops = 0;
    scene_set_paint(&a, rect, &redp);

    scene_render_frame(&r, &a, root, &rt);        /* frame 1: rect at P1 (4,4) */
    CHECK(red_at(&rt, 10, 10) > 200, "frame 1: P1 shows the rect");

    scene_set_transform(&a, rect, 40,40,0, 0,0,0,1, 1,1,1);   /* move to P2 */
    scene_render_frame(&r, &a, root, &rt);
    int p1_r = red_at(&rt, 10, 10), p1_g = green_at(&rt, 10, 10);
    int p2_r = red_at(&rt, 46, 46);
    CHECK(p2_r > 200, "frame 2: P2 shows the rect at its new position");
    CHECK(p1_r < 160 && p1_g > 100, "frame 2: P1 restored to background grey (no ghost)");

    free(rt.pixels); scene_render_destroy(&r); scene_arena_destroy(&a);
}

int main(void) {
    printf("=== EmbLink UI Piece 4a: render-backend selftests ===\n");
    t1_aa();
    t2_backdrop();
    t3_opacity_group();
    t4_dirty_blur();
    t5_ghost();
    printf("=== backend-test: %s (%d failures) ===\n", g_fail ? "FAIL" : "OK", g_fail);
    return g_fail ? 1 : 0;
}
