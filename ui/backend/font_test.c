/* ui/backend/font_test.c -- EmbLink UI Piece 4b selftests (Section 7).
 *
 * Pure userland. Hand-constructs a minimal valid .ttf in-memory (a table
 * directory + head/maxp/hhea/hmtx/cmap(fmt4)/loca/glyf with a square glyph, a
 * curved glyph, and a composite glyph) so the parser/rasterizer is tested
 * against KNOWN expected output, with no dependency on any external font.
 *   make font-test   -> exits 0 iff every T1..T5 invariant holds. */

#include "font.h"
#include "scene_render.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  FAIL: %s\n", (msg)); g_fail++; } \
    else         { printf("  ok:   %s\n", (msg)); } \
} while (0)

/* ---- big-endian byte writer over a fixed buffer ------------------------ */
static uint8_t  g_ttf[4096];
static uint32_t g_len;
static void w8(uint8_t v)  { g_ttf[g_len++] = v; }
static void w16(uint16_t v){ w8((uint8_t)(v>>8)); w8((uint8_t)v); }
static void w32(uint32_t v){ w16((uint16_t)(v>>16)); w16((uint16_t)v); }
static void wi16(int16_t v){ w16((uint16_t)v); }
static void w16_at(uint32_t off, uint16_t v){ g_ttf[off]=(uint8_t)(v>>8); g_ttf[off+1]=(uint8_t)v; }
static void w32_at(uint32_t off, uint32_t v){ w16_at(off,(uint16_t)(v>>16)); w16_at(off+2,(uint16_t)v); }

static int alpha_px(struct render_target *rt, int x, int y) {
    if (x < 0 || y < 0 || x >= (int)rt->width || y >= (int)rt->height) return 0;
    uint32_t p = ((uint32_t *)((unsigned char *)rt->pixels + (size_t)y * rt->stride))[x];
    return (int)((p >> 24) & 255);
}

/* ---- build the synthetic font; returns total length ------------------- */
static uint32_t build_ttf(void) {
    g_len = 0;
    const int n_tables = 7;

    /* ---- offset table + (empty) directory we backpatch ---- */
    w32(0x00010000);            /* sfnt version */
    w16(n_tables);
    w16(0); w16(0); w16(0);     /* searchRange/entrySelector/rangeShift (unused by parser) */
    uint32_t dir = g_len;
    for (int i = 0; i < n_tables; i++) { w32(0); w32(0); w32(0); w32(0); }  /* 16 bytes each */

    struct { const char *tag; uint32_t off, len; } rec[7];
    int ri = 0;
    #define BEGIN(TAG) do { rec[ri].tag = (TAG); rec[ri].off = g_len; } while (0)
    #define END()      do { rec[ri].len = g_len - rec[ri].off; \
                            while (g_len & 3) { w8(0); } ri++; } while (0)

    /* head (54 bytes): unitsPerEm@18, indexToLocFormat@50 */
    BEGIN("head");
    { uint32_t s = g_len; for (int i=0;i<54;i++) w8(0);
      w32_at(s+0, 0x00010000);            /* version */
      w16_at(s+18, 1000);                 /* unitsPerEm */
      w16_at(s+50, 1); }                  /* indexToLocFormat = 1 (uint32 loca) */
    END();

    /* maxp (6 bytes): numGlyphs@4 = 4 */
    BEGIN("maxp");
    w32(0x00010000); w16(4);
    END();

    /* hhea (36 bytes): numberOfHMetrics@34 = 4 */
    BEGIN("hhea");
    { uint32_t s = g_len; for (int i=0;i<36;i++) w8(0); w16_at(s+34, 4); }
    END();

    /* hmtx: 4 longHorMetric {advanceWidth, lsb} */
    BEGIN("hmtx");
    w16(500); w16(100);   /* glyph 0 .notdef */
    w16(500); w16(100);   /* glyph 1 square  */
    w16(500); w16(100);   /* glyph 2 curve   */
    w16(550); w16(100);   /* glyph 3 composite */
    END();

    /* cmap: header + one (3,1) format-4 subtable. 'A'->1 'B'->2 'C'->3 */
    BEGIN("cmap");
    { uint32_t cmap_start = g_len;
      w16(0); w16(1);                       /* version, numTables */
      w16(3); w16(1); w32(12);              /* platform 3, enc 1, offset 12 from cmap start */
      uint32_t sub = g_len; (void)sub;      /* == cmap_start + 12 */
      /* format 4, segCount = 2 (real seg + 0xFFFF terminator) */
      w16(4);                               /* format */
      uint32_t lenoff = g_len; w16(0);      /* length (backpatch) */
      w16(0);                               /* language */
      w16(4);                               /* segCountX2 */
      w16(4); w16(1); w16(0);               /* searchRange/entrySelector/rangeShift */
      w16(0x0043); w16(0xFFFF);             /* endCode[]   */
      w16(0);                               /* reservedPad */
      w16(0x0041); w16(0xFFFF);             /* startCode[] */
      wi16(-64);  w16(1);                   /* idDelta[] : 0x41-64=1, 0x42..=2, 0x43..=3 */
      w16(0); w16(0);                       /* idRangeOffset[] (0 => direct) */
      w16_at(lenoff, (uint16_t)(g_len - cmap_start - 12)); /* subtable length */
    }
    END();

    /* glyf: glyph0 empty, glyph1 square, glyph2 curve, glyph3 composite */
    uint32_t g_off[5];
    BEGIN("glyf");
    { uint32_t glyf_start = g_len;
      g_off[0] = g_len - glyf_start;        /* glyph 0: empty */
      g_off[1] = g_len - glyf_start;

      /* glyph 1: square (100,100)-(400,400), 4 on-curve points */
      wi16(1);                              /* numberOfContours */
      wi16(100); wi16(100); wi16(400); wi16(400);   /* bbox */
      w16(3);                               /* endPtsOfContours[0] */
      w16(0);                               /* instructionLength */
      w8(0x01); w8(0x01); w8(0x01); w8(0x01);        /* flags: all ON_CURVE, int16 deltas */
      wi16(100); wi16(300); wi16(0); wi16(-300);     /* x deltas */
      wi16(100); wi16(0); wi16(300); wi16(0);        /* y deltas */
      g_off[2] = g_len - glyf_start;

      /* glyph 2: quadratic curve, on(100,100) off(250,400) on(400,100) */
      wi16(1);
      wi16(100); wi16(100); wi16(400); wi16(400);
      w16(2);                               /* endPts: 3 points (0..2) */
      w16(0);
      w8(0x01); w8(0x00); w8(0x01);         /* on, OFF-curve, on */
      wi16(100); wi16(150); wi16(150);      /* x: 100,250,400 */
      wi16(100); wi16(300); wi16(-300);     /* y: 100,400,100 */
      g_off[3] = g_len - glyf_start;

      /* glyph 3: composite -> glyph 1 offset by (50,25) */
      wi16(-1);
      wi16(150); wi16(125); wi16(450); wi16(425);
      w16(0x0003);                          /* ARG_1_AND_2_ARE_WORDS | ARGS_ARE_XY_VALUES */
      w16(1);                               /* component glyphIndex */
      wi16(50); wi16(25);                   /* dx, dy */
      g_off[4] = g_len - glyf_start;
    }
    END();

    /* loca: 5 uint32 offsets (index_to_loc_fmt = 1) */
    BEGIN("loca");
    for (int i = 0; i < 5; i++) w32(g_off[i]);
    END();

    /* backpatch the directory */
    for (int i = 0; i < n_tables; i++) {
        uint32_t r = dir + (uint32_t)i * 16;
        g_ttf[r]   = (uint8_t)rec[i].tag[0]; g_ttf[r+1] = (uint8_t)rec[i].tag[1];
        g_ttf[r+2] = (uint8_t)rec[i].tag[2]; g_ttf[r+3] = (uint8_t)rec[i].tag[3];
        w32_at(r + 4, 0);                   /* checksum (unvalidated) */
        w32_at(r + 8, rec[i].off);
        w32_at(r + 12, rec[i].len);
    }
    return g_len;
}

/* ---- T1: parse + rasterize a known square ------------------------------ */
static uint32_t t1_load_and_square(void) {
    printf("T1 parse + rasterize a synthetic square:\n");
    uint32_t len = build_ttf();
    uint32_t fh = font_load(g_ttf, len);
    CHECK(fh != 0, "font_load succeeds on the synthetic ttf");
    struct font *f = font_for_handle(fh);
    CHECK(f != 0 && f->units_per_em == 1000, "head parsed (units_per_em=1000)");
    CHECK(f && f->num_glyphs == 4, "maxp parsed (num_glyphs=4)");

    CHECK(font_codepoint_to_glyph(f, 'A') == 1, "cmap: 'A' -> glyph 1");
    CHECK(font_codepoint_to_glyph(f, 'B') == 2, "cmap: 'B' -> glyph 2");
    CHECK(font_codepoint_to_glyph(f, 'Z') == 0, "cmap: unmapped 'Z' -> .notdef 0");

    struct glyph_outline o;
    CHECK(font_get_glyph_outline(f, 1, &o), "get square outline");
    CHECK(o.n_contours == 1 && o.contours[0].n_points == 4, "square: 1 contour, 4 points");
    int all_on = 1;
    for (uint32_t i = 0; i < o.contours[0].n_points; i++) if (!o.contours[0].points[i].on_curve) all_on = 0;
    CHECK(all_on, "square: all points on-curve");
    font_free_outline(&o);

    uint8_t *cov = 0; int w=0,h=0,bx=0,by=0; float adv=0;
    CHECK(font_rasterize_glyph(f, 1, 100.0f, &cov, &w, &h, &bx, &by, &adv), "rasterize square @100px");
    /* 300 font units * (100/1000) = 30px square */
    CHECK(w >= 28 && w <= 32 && h >= 28 && h <= 32, "square bitmap ~30x30");
    CHECK(cov && cov[(h/2)*w + w/2] >= 250, "square center fully covered");
    /* a corner-ish sample inside, and outside-of-bitmap is by definition uncovered */
    CHECK(cov && cov[2*w + 2] >= 250, "square interior covered");
    free(cov);
    return fh;
}

/* ---- T2: quadratic flattening actually curves -------------------------- */
static void t2_curve(struct font *f) {
    printf("T2 quadratic curve flattening:\n");
    struct glyph_outline o;
    CHECK(font_get_glyph_outline(f, 2, &o), "get curve outline");
    int has_off = 0;
    for (uint32_t i = 0; i < o.contours[0].n_points; i++) if (!o.contours[0].points[i].on_curve) has_off = 1;
    CHECK(has_off, "curve glyph has an off-curve control point");
    font_free_outline(&o);

    /* flatten contour 0 and confirm the polyline bulges off the endpoint chord.
     * Endpoints share y=100 (font units); the control at y=400 pulls the curve
     * to y~250 at its midpoint -- a straight-line replay would keep y flat. */
    float xs[512], ys[512];
    int n = font_debug_flatten_contour0(f, 2, 100.0f / 1000.0f, xs, ys, 512);
    CHECK(n > 4, "flattening produced intermediate polyline points");
    float ymin = 1e30f, ymax = -1e30f;
    for (int i = 0; i < n; i++) { if (ys[i] < ymin) ymin = ys[i]; if (ys[i] > ymax) ymax = ys[i]; }
    /* y spread in pixels should be ~ (250-100)*0.1 = 15px if the curve bulges;
     * a straight chord (all endpoints y=100) would spread ~0. */
    CHECK((ymax - ymin) > 8.0f, "flattened curve deviates from the straight chord");
}

/* ---- T3: composite glyph offset ---------------------------------------- */
static void t3_composite(struct font *f) {
    printf("T3 composite glyph offset:\n");
    struct glyph_outline sq, comp;
    CHECK(font_get_glyph_outline(f, 1, &sq), "get base square");
    CHECK(font_get_glyph_outline(f, 3, &comp), "get composite");
    CHECK(comp.n_contours == 1 && comp.contours[0].n_points == 4, "composite: 1 contour, 4 points (from base)");
    int shifted_ok = 1;
    for (uint32_t i = 0; i < 4 && i < comp.contours[0].n_points; i++) {
        float ex = sq.contours[0].points[i].x + 50.0f;
        float ey = sq.contours[0].points[i].y + 25.0f;
        if (comp.contours[0].points[i].x != ex || comp.contours[0].points[i].y != ey) shifted_ok = 0;
    }
    CHECK(shifted_ok, "composite points == base square shifted by exactly (50,25)");
    font_free_outline(&sq); font_free_outline(&comp);
}

/* ---- T4: atlas caching (no re-rasterize on repeat lookup) -------------- */
static void t4_atlas_cache(struct font *f) {
    printf("T4 atlas cache is a real cache:\n");
    struct glyph_atlas *atlas = font_global_atlas();
    uint32_t before = font_debug_rasterize_count();
    struct glyph_cache_entry *e1 = glyph_cache_lookup_or_rasterize(atlas, f, 'A', 32.0f);
    uint32_t mid = font_debug_rasterize_count();
    struct glyph_cache_entry *e2 = glyph_cache_lookup_or_rasterize(atlas, f, 'A', 32.0f);
    uint32_t after = font_debug_rasterize_count();

    CHECK(e1 && e2, "both lookups returned an entry");
    CHECK(mid == before + 1, "first lookup rasterized once");
    CHECK(after == mid, "second lookup did NOT rasterize (cache hit)");
    CHECK(e1->atlas_x == e2->atlas_x && e1->atlas_y == e2->atlas_y, "same atlas slot on hit");
}

/* ---- T5: pen advance + multi-glyph layout ------------------------------ */
static void t5_pen_advance(struct font *f, uint32_t fh) {
    printf("T5 pen advance + multi-glyph layout:\n");
    font_install_backend();
    struct render_backend *be = cpu_backend_get();
    CHECK(be->draw_text != 0, "draw_text installed into the vtable");

    /* render "AAA" and confirm three inked squares spaced by the advance. */
    int W = 128, H = 64;
    struct render_target rt;
    rt.pixels = calloc((size_t)W*H, 4); rt.width = W; rt.height = H; rt.stride = W*4;
    rt.format = EMBK_PIXFMT_BGRA8888_PRE;
    be->begin_frame(&rt, NULL, 0);
    struct color white = {1,1,1,1};
    be->draw_text(&rt, 4, 40, "AAA", fh, 50.0f, white, 1.0f);
    be->end_frame(&rt);

    /* advance = 500 * 50/1000 = 25px; square 'A' ~ x in [bearing, bearing+15] */
    struct glyph_cache_entry *g = glyph_cache_lookup_or_rasterize(font_global_atlas(), f, 'A', 50.0f);
    int adv = (int)(g->advance_px + 0.5f);
    CHECK(adv >= 24 && adv <= 26, "advance ~25px");

    int cy = 25;   /* somewhere inside the glyph rows */
    int a0 = alpha_px(&rt, 4 + g->bearing_x + 5, cy);
    int a1 = alpha_px(&rt, 4 + adv + g->bearing_x + 5, cy);
    int a2 = alpha_px(&rt, 4 + 2*adv + g->bearing_x + 5, cy);
    CHECK(a0 > 40, "glyph 0 inked at pen origin");
    CHECK(a1 > 40, "glyph 1 inked one advance over");
    CHECK(a2 > 40, "glyph 2 inked two advances over");

    free(rt.pixels);
}

int main(void) {
    printf("=== EmbLink UI Piece 4b: font-rasterizer selftests ===\n");
    uint32_t fh = t1_load_and_square();
    struct font *f = font_for_handle(fh);
    if (f) {
        t2_curve(f);
        t3_composite(f);
        t4_atlas_cache(f);
        t5_pen_advance(f, fh);
    } else { printf("  FAIL: no font handle; skipping T2-T5\n"); g_fail++; }
    printf("=== font-test: %s (%d failures) ===\n", g_fail ? "FAIL" : "OK", g_fail);
    return g_fail ? 1 : 0;
}
