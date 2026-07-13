#ifndef __EMBLINK_UI_FONT_H__
#define __EMBLINK_UI_FONT_H__

/* ui/backend/font.h -- EmbLink UI Piece 4b: the font rasterizer.
 *
 * Fills in Piece 4a's declared-but-NULL draw_text vtable entry. Pure userland
 * C, no kernel involvement. Direction (a) from the 4a fork: a full runtime
 * TrueType (glyf-outline) parser + rasterizer, entirely inside EmbLink, made
 * affordable by a glyph atlas so each (font, codepoint, size) is rasterized
 * exactly once ever.
 *
 * Scope (Section 0, decided not asked): TrueType glyf only (no CFF/OpenType);
 * no hinting; supersample+box-downsample AA; cmap format 4 (BMP); simple
 * offset/scale composites; no kerning; no shaping (LTR, 1 codepoint -> 1
 * glyph). Each omission is a named gap (Section 8), not an oversight. */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "backend.h"    /* struct render_target, struct color */

/* ------------------------------------------------------------------------- */
/* Section 1: the parsed font                                                */
/* ------------------------------------------------------------------------- */

struct font {
    const uint8_t *data;        /* caller-owned, resident for the font's lifetime */
    size_t          data_len;

    uint16_t units_per_em;       /* 'head' -- outline coord scale; px = coord * size_px/units_per_em */
    int16_t  index_to_loc_fmt;   /* 'head' -- 0: loca is uint16*2, 1: uint32 */
    uint16_t num_glyphs;         /* 'maxp' */

    uint32_t glyf_offset, glyf_len;
    uint32_t loca_offset, loca_len;
    uint32_t cmap_subtable_offset;  /* the chosen FORMAT-4 subtable itself */
    uint32_t hmtx_offset;
    uint16_t num_h_metrics;         /* 'hhea' */
    /* vertical metrics ('hhea', same table as num_h_metrics). ascent/line_gap
     * as-read; descent is stored POSITIVE (negated from the file's negative
     * descender) so line_height = (ascent + descent + line_gap) * scale --
     * Piece 5's layout line-height formula. */
    int16_t  ascent, descent, line_gap;
    bool     used;
};

/* Parse the sfnt table directory once; -> a handle (index into a small font
 * table, 1-based; 0 == failure). No checksum validation -- later bounds checks
 * fail the specific lookup, not the load. Nothing is rasterized here. */
uint32_t font_load(const uint8_t *ttf_data, size_t len);
struct font *font_for_handle(uint32_t handle);

/* ------------------------------------------------------------------------- */
/* Section 2: cmap (format 4)                                                */
/* ------------------------------------------------------------------------- */

/* -> glyph index, or 0 (.notdef) if `codepoint` isn't in the format-4 map. */
uint32_t font_codepoint_to_glyph(struct font *f, uint32_t codepoint);

/* ------------------------------------------------------------------------- */
/* Section 3: glyf/loca outlines                                             */
/* ------------------------------------------------------------------------- */

struct glyph_point   { float x, y; bool on_curve; };
struct glyph_contour { struct glyph_point *points; uint32_t n_points; };

struct glyph_outline {
    struct glyph_contour *contours;
    uint32_t               n_contours;
    int16_t  x_min, y_min, x_max, y_max;   /* font units */
    uint16_t advance_width;                 /* font units, from hmtx */
    int16_t  left_side_bearing;
};

/* Extract glyph `glyph_index`'s outline (simple or composite). Allocates
 * contours/points; free with font_free_outline. An empty range (space) is a
 * valid outline with n_contours == 0. */
bool font_get_glyph_outline(struct font *f, uint32_t glyph_index, struct glyph_outline *out);
void font_free_outline(struct glyph_outline *o);

/* ------------------------------------------------------------------------- */
/* Section 4: rasterization (outline -> 8-bit coverage bitmap)               */
/* ------------------------------------------------------------------------- */

/* out_coverage: freshly malloc'd out_w*out_h bytes (single 8-bit alpha). The
 * caller copies it into the atlas and frees it. bearing/advance in PIXELS. */
bool font_rasterize_glyph(struct font *f, uint32_t glyph_index, float size_px,
                          uint8_t **out_coverage, int *out_w, int *out_h,
                          int *out_bearing_x, int *out_bearing_y, float *out_advance_px);

/* Debug counter: how many times font_rasterize_glyph actually ran (T4 asserts
 * the cache stops this incrementing on a repeat lookup). */
uint32_t font_debug_rasterize_count(void);

/* Debug: flatten contour 0 of glyph `gi` to a pixel-space polyline (T2 checks
 * the curve actually bulges off its endpoint chord). Returns the point count;
 * copies up to `max` into xs/ys. */
int font_debug_flatten_contour0(struct font *f, uint32_t gi, float scale,
                                float *xs, float *ys, int max);

/* ------------------------------------------------------------------------- */
/* Section 5: glyph atlas (cache)                                            */
/* ------------------------------------------------------------------------- */

#define GLYPH_ATLAS_SIZE  1024   /* single 8-bit alpha texture */
#define GLYPH_CACHE_MAX   4096   /* distinct (font,codepoint,size) entries */

struct glyph_cache_entry {
    uint32_t font_handle, codepoint;
    uint16_t size_px_x4;          /* size_px * 4 rounded -> collapses float noise */
    uint16_t atlas_x, atlas_y, atlas_w, atlas_h;
    int16_t  bearing_x, bearing_y;
    float    advance_px;
    bool     used;
};

struct glyph_atlas {
    uint8_t  coverage[GLYPH_ATLAS_SIZE][GLYPH_ATLAS_SIZE];
    uint32_t shelf_x, shelf_y, shelf_row_h;   /* shelf/row packer cursor */
    struct glyph_cache_entry entries[GLYPH_CACHE_MAX];
    uint32_t n_entries;
};

/* Lookup by (font_handle, codepoint, size_px_x4); on miss, rasterize + pack.
 * On atlas/cache exhaustion: full clear-and-restart (Section 5). */
struct glyph_cache_entry *glyph_cache_lookup_or_rasterize(
    struct glyph_atlas *atlas, struct font *f, uint32_t codepoint, float size_px);

/* ------------------------------------------------------------------------- */
/* Section 6: draw_text -- installs into the CPU backend's vtable slot        */
/* ------------------------------------------------------------------------- */

/* Point cpu_backend_get()->draw_text at the real implementation. Call once at
 * startup (the vtable ships with draw_text == NULL until then). */
void font_install_backend(void);

/* Exposed for tests: the shared process-wide atlas draw_text uses. */
struct glyph_atlas *font_global_atlas(void);

#endif /* __EMBLINK_UI_FONT_H__ */
