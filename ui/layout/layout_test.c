/* ui/layout/layout_test.c -- EmbLink UI Piece 5 selftests (Section 5).
 *
 * Pure userland. Builds a compact synthetic font (empty glyphs, real advances
 * via hmtx + real hhea ascent/descent/line_gap) so text measurement has known
 * metrics without any glyph outlines. Layout nodes pair with real Piece-3
 * scene nodes so the write-back (T6) is exercised for real.
 *   make layout-test  -> exits 0 iff every T1..T6 invariant holds. */

#include "layout.h"
#include "scene.h"
#include "font.h"
#include <stdio.h>
#include <stdlib.h>

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  FAIL: %s\n", (msg)); g_fail++; } \
    else         { printf("  ok:   %s\n", (msg)); } \
} while (0)
static int feq(float a, float b) { float d = a-b; if (d<0) d=-d; return d < 0.05f; }

static struct scene_arena  SA;
static struct layout_arena LA;

/* pair a new layout node with a new scene node under the given parents */
static struct layout_handle mk(struct layout_handle lp, struct node_handle sp,
                               enum scene_node_kind kind, struct node_handle *out_s) {
    struct node_handle s = scene_create_node(&SA, kind, sp);
    struct layout_handle l = layout_create_node(&LA, lp);
    struct layout_node *ln = layout_resolve(&LA, l);
    ln->scene_node = s;
    if (out_s) *out_s = s;
    return l;
}
static struct layout_node *L(struct layout_handle h) { return layout_resolve(&LA, h); }

/* ---- synthetic font: all-empty glyphs, advance 500, hhea 800/-200/0 ---- */
static uint8_t g_ttf[8192]; static uint32_t g_len;
static void w8(uint8_t v){ g_ttf[g_len++]=v; }
static void w16(uint16_t v){ w8(v>>8); w8(v); }
static void w32(uint32_t v){ w16(v>>16); w16(v); }
static void wi16(int16_t v){ w16((uint16_t)v); }
static void w16_at(uint32_t o,uint16_t v){ g_ttf[o]=v>>8; g_ttf[o+1]=v; }
static void w32_at(uint32_t o,uint32_t v){ w16_at(o,v>>16); w16_at(o+2,v); }

static uint32_t build_font(void) {
    g_len = 0;
    const int nt = 7;
    w32(0x00010000); w16(nt); w16(0); w16(0); w16(0);
    uint32_t dir = g_len;
    for (int i=0;i<nt;i++){ w32(0);w32(0);w32(0);w32(0); }
    struct { const char *tag; uint32_t off,len; } rec[7]; int ri=0;
    #define BEG(T) do{ rec[ri].tag=(T); rec[ri].off=g_len; }while(0)
    #define ENDT() do{ rec[ri].len=g_len-rec[ri].off; while(g_len&3){ w8(0); } ri++; }while(0)

    BEG("head"); { uint32_t s=g_len; for(int i=0;i<54;i++) w8(0);
        w32_at(s+0,0x00010000); w16_at(s+18,1000); w16_at(s+50,1); } ENDT();
    BEG("maxp"); w32(0x00010000); w16(128); ENDT();
    BEG("hhea"); { uint32_t s=g_len; for(int i=0;i<36;i++) w8(0);
        w16_at(s+4,800); w16_at(s+6,(uint16_t)(int16_t)-200); w16_at(s+8,0); w16_at(s+34,1); } ENDT();
    BEG("hmtx"); w16(500); w16(0); ENDT();                 /* 1 metric: advance 500 */
    BEG("cmap"); { uint32_t cs=g_len;
        w16(0); w16(1); w16(3); w16(1); w32(12);
        w16(4); uint32_t lo=g_len; w16(0); w16(0); w16(4); w16(4); w16(1); w16(0);
        w16(0x007e); w16(0xFFFF);           /* endCode */
        w16(0);                              /* pad */
        w16(0x0020); w16(0xFFFF);            /* startCode */
        wi16(0); w16(1);                     /* idDelta: glyph = codepoint */
        w16(0); w16(0);                      /* idRangeOffset */
        w16_at(lo,(uint16_t)(g_len-cs-12)); } ENDT();
    BEG("glyf"); ENDT();                                    /* empty: all glyphs blank */
    BEG("loca"); for(int i=0;i<129;i++) w32(0); ENDT();     /* every glyph empty */

    for (int i=0;i<nt;i++){ uint32_t r=dir+i*16;
        g_ttf[r]=rec[i].tag[0]; g_ttf[r+1]=rec[i].tag[1]; g_ttf[r+2]=rec[i].tag[2]; g_ttf[r+3]=rec[i].tag[3];
        w32_at(r+4,0); w32_at(r+8,rec[i].off); w32_at(r+12,rec[i].len); }
    return g_len;
}

/* ---- T1: SPACE_BETWEEN distribution ------------------------------------ */
static void t1_space_between(void) {
    printf("T1 fixed children, justify SPACE_BETWEEN:\n");
    scene_arena_init(&SA); layout_arena_init(&LA);
    struct layout_handle root = mk(LAYOUT_HANDLE_NULL, NODE_HANDLE_NULL, SCENE_NODE_GROUP, 0);
    L(root)->is_container = true; L(root)->axis = AXIS_ROW; L(root)->justify = JUSTIFY_SPACE_BETWEEN;
    struct layout_handle c1 = mk(root, NODE_HANDLE_NULL, SCENE_NODE_RECT, 0);
    struct layout_handle c2 = mk(root, NODE_HANDLE_NULL, SCENE_NODE_RECT, 0);
    struct layout_handle c3 = mk(root, NODE_HANDLE_NULL, SCENE_NODE_RECT, 0);
    L(c1)->width = (struct layout_size){ SIZE_FIXED, 10, 0,0,0 }; L(c1)->height = (struct layout_size){ SIZE_FIXED,10,0,0,0 };
    L(c2)->width = (struct layout_size){ SIZE_FIXED, 20, 0,0,0 }; L(c2)->height = (struct layout_size){ SIZE_FIXED,10,0,0,0 };
    L(c3)->width = (struct layout_size){ SIZE_FIXED, 30, 0,0,0 }; L(c3)->height = (struct layout_size){ SIZE_FIXED,10,0,0,0 };

    layout_run(&LA, &SA, root, 100, 50);
    CHECK(feq(L(c1)->resolved_x, 0),  "child 1 at x=0");
    CHECK(feq(L(c2)->resolved_x, 30), "child 2 at x=30 (10 + 20 gap)");
    CHECK(feq(L(c3)->resolved_x, 70), "child 3 at x=70");
    CHECK(feq(L(c3)->resolved_x + L(c3)->resolved_w, 100), "child 3 right edge at x=100");
    layout_arena_destroy(&LA); scene_arena_destroy(&SA);
}

/* ---- T2: flex_grow exact numeric --------------------------------------- */
static void t2_grow(void) {
    printf("T2 flex_grow distribution:\n");
    scene_arena_init(&SA); layout_arena_init(&LA);
    struct layout_handle root = mk(LAYOUT_HANDLE_NULL, NODE_HANDLE_NULL, SCENE_NODE_GROUP, 0);
    L(root)->is_container = true; L(root)->axis = AXIS_ROW;
    struct layout_handle a = mk(root, NODE_HANDLE_NULL, SCENE_NODE_RECT, 0);
    struct layout_handle b = mk(root, NODE_HANDLE_NULL, SCENE_NODE_RECT, 0);
    L(a)->width = (struct layout_size){ SIZE_FIXED, 20, 1,0,0 };
    L(b)->width = (struct layout_size){ SIZE_FIXED, 10, 2,0,0 };
    layout_run(&LA, &SA, root, 100, 50);
    /* remaining 70 split 1:2 over bases 20/10 */
    CHECK(feq(L(a)->resolved_w, 20 + 70.0f/3.0f), "A width = 20 + 70/3 (~43.33)");
    CHECK(feq(L(b)->resolved_w, 10 + 140.0f/3.0f), "B width = 10 + 70*2/3 (~56.67)");
    CHECK(feq(L(a)->resolved_w + L(b)->resolved_w, 100), "widths sum to exactly 100");
    layout_arena_destroy(&LA); scene_arena_destroy(&SA);
}

/* ---- T3: CSS-accurate size-weighted flex_shrink ------------------------ */
static void t3_shrink(void) {
    printf("T3 flex_shrink (size-weighted, not equal split):\n");
    scene_arena_init(&SA); layout_arena_init(&LA);
    struct layout_handle root = mk(LAYOUT_HANDLE_NULL, NODE_HANDLE_NULL, SCENE_NODE_GROUP, 0);
    L(root)->is_container = true; L(root)->axis = AXIS_ROW;
    struct layout_handle a = mk(root, NODE_HANDLE_NULL, SCENE_NODE_RECT, 0);
    struct layout_handle b = mk(root, NODE_HANDLE_NULL, SCENE_NODE_RECT, 0);
    L(a)->width = (struct layout_size){ SIZE_FIXED, 60, 0,1,0 };
    L(b)->width = (struct layout_size){ SIZE_FIXED, 40, 0,1,0 };
    layout_run(&LA, &SA, root, 80, 50);   /* overflow 20; weights 60 and 40 */
    CHECK(feq(L(a)->resolved_w, 48), "A shrinks by 20*60/100=12 -> 48 (NOT 50)");
    CHECK(feq(L(b)->resolved_w, 32), "B shrinks by 20*40/100=8  -> 32 (NOT 50)");
    layout_arena_destroy(&LA); scene_arena_destroy(&SA);
}

/* ---- T4: ALIGN_STRETCH stretches AUTO cross sizes; FIXED always wins ---- */
/* (CSS semantics: stretch applies only to auto-sized items. A definite cross
 * size is never overridden -- relied on by declare's hit-test clip test once
 * the ui root became a stretch column.) */
static void t4_stretch(void) {
    printf("T4 ALIGN_STRETCH cross-size rules:\n");
    scene_arena_init(&SA); layout_arena_init(&LA);
    struct layout_handle root = mk(LAYOUT_HANDLE_NULL, NODE_HANDLE_NULL, SCENE_NODE_GROUP, 0);
    L(root)->is_container = true; L(root)->axis = AXIS_COLUMN; L(root)->align = ALIGN_STRETCH;
    struct layout_handle c = mk(root, NODE_HANDLE_NULL, SCENE_NODE_RECT, 0);
    L(c)->width  = (struct layout_size){ SIZE_INTRINSIC, 0, 0,0,0 };  /* auto cross-width */
    L(c)->height = (struct layout_size){ SIZE_FIXED, 10, 0,0,0 };
    struct layout_handle f = mk(root, NODE_HANDLE_NULL, SCENE_NODE_RECT, 0);
    L(f)->width  = (struct layout_size){ SIZE_FIXED, 10, 0,0,0 };     /* definite cross-width */
    L(f)->height = (struct layout_size){ SIZE_FIXED, 10, 0,0,0 };
    layout_run(&LA, &SA, root, 50, 40);   /* parent cross-width 50 */
    CHECK(feq(L(c)->resolved_w, 50), "auto-width child stretches to the parent's 50");
    CHECK(feq(L(f)->resolved_w, 10), "FIXED-width child keeps its 10 (stretch never overrides definite)");
    layout_arena_destroy(&LA); scene_arena_destroy(&SA);
}

/* ---- T5: text wrapping (word-wrap + character fallback) ---------------- */
static void t5_wrap(uint32_t fh) {
    printf("T5 text wrapping:\n");
    scene_arena_init(&SA); layout_arena_init(&LA);
    struct node_handle stext;
    struct layout_handle t = mk(LAYOUT_HANDLE_NULL, NODE_HANDLE_NULL, SCENE_NODE_TEXT, &stext);
    struct color black = {0,0,0,1};
    scene_set_text(&SA, stext, "aa aa aa", fh, 20.0f, black);   /* 3 words, 20px each; space 10px */

    /* at width 25: each 20px word forces its own line -> 3 lines, 3*20px=60 */
    int lines = layout_debug_wrap_lines(&LA, &SA, t, 25.0f);
    float h = layout_measure_height_at_width(&LA, &SA, t, 25.0f);
    CHECK(lines == 3, "word-wrap: 3 lines at width 25");
    CHECK(feq(h, 60.0f), "height == 3 * line_height (20px)");

    /* single unbroken over-wide word must character-wrap, never overflow */
    scene_set_text(&SA, stext, "aaaaaa", fh, 20.0f, black);      /* 60px, no spaces */
    int clines = layout_debug_wrap_lines(&LA, &SA, t, 25.0f);
    CHECK(clines > 1, "character-fallback wraps a single overlong word");
    layout_arena_destroy(&LA); scene_arena_destroy(&SA);
}

/* ---- T6: end-to-end write-back into the scene tree --------------------- */
static void t6_writeback(uint32_t fh) {
    printf("T6 end-to-end write-back into scene tree:\n");
    scene_arena_init(&SA); layout_arena_init(&LA);
    struct node_handle s_row, s_col, s_text;
    struct layout_handle row = mk(LAYOUT_HANDLE_NULL, NODE_HANDLE_NULL, SCENE_NODE_GROUP, &s_row);
    L(row)->is_container = true; L(row)->axis = AXIS_ROW; L(row)->padding_left = 5; L(row)->padding_top = 3;
    struct layout_handle col = mk(row, s_row, SCENE_NODE_GROUP, &s_col);
    L(col)->is_container = true; L(col)->axis = AXIS_COLUMN;
    struct layout_handle txt = mk(col, s_col, SCENE_NODE_TEXT, &s_text);
    struct color black = {0,0,0,1};
    scene_set_text(&SA, s_text, "hi", fh, 20.0f, black);

    layout_run(&LA, &SA, row, 200, 100);

    struct layout_node *lt = L(txt);
    struct scene_node *st = scene_resolve(&SA, s_text);
    CHECK(st != 0, "deepest text scene node resolves");
    CHECK(st && feq(st->width, lt->resolved_w) && feq(st->height, lt->resolved_h),
          "scene node size == layout resolved size");
    CHECK(st && feq(st->tx, lt->resolved_x) && feq(st->ty, lt->resolved_y),
          "scene node transform == layout resolved position");
    /* and it's genuinely nonzero geometry, not both trivially 0 */
    CHECK(lt->resolved_w > 0 && lt->resolved_h > 0, "text resolved to real (nonzero) geometry");
    layout_arena_destroy(&LA); scene_arena_destroy(&SA);
}

int main(void) {
    printf("=== EmbLink UI Piece 5: layout-engine selftests ===\n");
    uint32_t fh = font_load(g_ttf, build_font());
    if (!fh) { printf("  FAIL: synthetic font_load\n"); return 1; }
    t1_space_between();
    t2_grow();
    t3_shrink();
    t4_stretch();
    t5_wrap(fh);
    t6_writeback(fh);
    printf("=== layout-test: %s (%d failures) ===\n", g_fail ? "FAIL" : "OK", g_fail);
    return g_fail ? 1 : 0;
}
