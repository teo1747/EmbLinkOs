/* ui/backend/font.c -- EmbLink UI Piece 4b: the TrueType font rasterizer
 * (see font.h). Runtime glyf-outline parsing + supersampled scan conversion,
 * cached in a shared glyph atlas so each (font,codepoint,size) rasterizes once.
 *
 * Host-buildable; a freestanding ring-3 port needs malloc + a couple of libm
 * calls shimmed (same host-first posture as Pieces 3/4a). */

#include "font.h"
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* big-endian sfnt readers (all bounds-checked against data_len by callers)   */
/* ------------------------------------------------------------------------- */

static uint16_t rd_u16(const uint8_t *d, uint32_t off) { return (uint16_t)((d[off] << 8) | d[off+1]); }
static int16_t  rd_i16(const uint8_t *d, uint32_t off) { return (int16_t)rd_u16(d, off); }
static uint32_t rd_u32(const uint8_t *d, uint32_t off) {
    return ((uint32_t)d[off] << 24) | ((uint32_t)d[off+1] << 16)
         | ((uint32_t)d[off+2] << 8) | (uint32_t)d[off+3];
}

/* libm-free floor/ceil so this file stays portable (freestanding-ready). */
static float floorf_(float v) { int i = (int)v; return (v < 0 && (float)i != v) ? (float)(i-1) : (float)i; }
static float ceilf_(float v)  { int i = (int)v; return (v > 0 && (float)i != v) ? (float)(i+1) : (float)i; }
static float sqrtf_(float v)  { if (v <= 0) return 0; float x = v; for (int i = 0; i < 8; i++) x = 0.5f*(x + v/x); return x; }
#define floorf floorf_
#define ceilf  ceilf_
#define sqrtf  sqrtf_

/* ------------------------------------------------------------------------- */
/* the small font table                                                      */
/* ------------------------------------------------------------------------- */

#define MAX_FONTS 8
static struct font g_fonts[MAX_FONTS];

struct font *font_for_handle(uint32_t handle) {
    if (handle == 0 || handle > MAX_FONTS) return 0;
    struct font *f = &g_fonts[handle - 1];
    return f->used ? f : 0;
}

/* Locate a table's (offset,length) in the directory. Returns 0 if absent. */
static int find_table(const uint8_t *d, size_t len, const char tag[4],
                      uint32_t *out_off, uint32_t *out_len) {
    if (len < 12) return 0;
    uint16_t num_tables = rd_u16(d, 4);
    uint32_t dir = 12;
    for (uint16_t i = 0; i < num_tables; i++) {
        uint32_t rec = dir + (uint32_t)i * 16;
        if (rec + 16 > len) return 0;
        if (d[rec] == (uint8_t)tag[0] && d[rec+1] == (uint8_t)tag[1] &&
            d[rec+2] == (uint8_t)tag[2] && d[rec+3] == (uint8_t)tag[3]) {
            uint32_t off = rd_u32(d, rec + 8), tl = rd_u32(d, rec + 12);
            if ((uint64_t)off + tl > len) return 0;
            *out_off = off; *out_len = tl;
            return 1;
        }
    }
    return 0;
}

/* Walk cmap's encoding records for a (3,1) Windows/Unicode BMP format-4
 * subtable; store its absolute offset. Returns 0 if none found. */
static int pick_cmap_format4(const uint8_t *d, size_t len, uint32_t cmap_off, uint32_t *out_sub) {
    if (cmap_off + 4 > len) return 0;
    uint16_t n = rd_u16(d, cmap_off + 2);
    uint32_t best = 0;
    for (uint16_t i = 0; i < n; i++) {
        uint32_t rec = cmap_off + 4 + (uint32_t)i * 8;
        if (rec + 8 > len) return 0;
        uint16_t plat = rd_u16(d, rec), enc = rd_u16(d, rec + 2);
        uint32_t sub = cmap_off + rd_u32(d, rec + 4);
        if (sub + 2 > len) continue;
        if (rd_u16(d, sub) != 4) continue;          /* format 4 only */
        if (plat == 3 && (enc == 1 || enc == 0)) { best = sub; break; }  /* prefer Windows BMP */
        if (best == 0) best = sub;                  /* fall back to any format-4 */
    }
    if (!best) return 0;
    *out_sub = best;
    return 1;
}

uint32_t font_load(const uint8_t *ttf_data, size_t len) {
    if (!ttf_data || len < 12) return 0;
    uint32_t ver = rd_u32(ttf_data, 0);
    if (ver != 0x00010000 && ver != 0x74727565 /* 'true' */) return 0;

    int slot = -1;
    for (int i = 0; i < MAX_FONTS; i++) if (!g_fonts[i].used) { slot = i; break; }
    if (slot < 0) return 0;
    struct font *f = &g_fonts[slot];
    memset(f, 0, sizeof(*f));
    f->data = ttf_data; f->data_len = len;

    uint32_t off, tl;
    if (!find_table(ttf_data, len, "head", &off, &tl) || tl < 54) return 0;
    f->units_per_em = rd_u16(ttf_data, off + 18);
    f->index_to_loc_fmt = rd_i16(ttf_data, off + 50);
    if (f->units_per_em == 0) return 0;

    if (!find_table(ttf_data, len, "maxp", &off, &tl) || tl < 6) return 0;
    f->num_glyphs = rd_u16(ttf_data, off + 4);

    if (!find_table(ttf_data, len, "hhea", &off, &tl) || tl < 36) return 0;
    f->ascent   = rd_i16(ttf_data, off + 4);
    f->descent  = (int16_t)(-rd_i16(ttf_data, off + 6));  /* file stores it negative */
    f->line_gap = rd_i16(ttf_data, off + 8);
    f->num_h_metrics = rd_u16(ttf_data, off + 34);

    if (!find_table(ttf_data, len, "hmtx", &off, &tl)) return 0;
    f->hmtx_offset = off;

    if (!find_table(ttf_data, len, "loca", &f->loca_offset, &f->loca_len)) return 0;
    if (!find_table(ttf_data, len, "glyf", &f->glyf_offset, &f->glyf_len)) return 0;

    uint32_t cmap_off, cmap_len;
    if (!find_table(ttf_data, len, "cmap", &cmap_off, &cmap_len)) return 0;
    if (!pick_cmap_format4(ttf_data, len, cmap_off, &f->cmap_subtable_offset)) return 0;

    f->used = true;
    return (uint32_t)(slot + 1);
}

/* ------------------------------------------------------------------------- */
/* Section 2: cmap format 4                                                  */
/* ------------------------------------------------------------------------- */

uint32_t font_codepoint_to_glyph(struct font *f, uint32_t codepoint) {
    if (!f || codepoint > 0xFFFF) return 0;
    const uint8_t *d = f->data;
    uint32_t s = f->cmap_subtable_offset;
    if (s + 14 > f->data_len) return 0;
    uint16_t segX2 = rd_u16(d, s + 6);
    uint16_t segs = segX2 / 2;
    uint32_t endCode   = s + 14;
    uint32_t startCode = endCode + segX2 + 2;   /* +2 reservedPad */
    uint32_t idDelta   = startCode + segX2;
    uint32_t idRange   = idDelta + segX2;
    if (idRange + segX2 > f->data_len) return 0;

    for (uint16_t i = 0; i < segs; i++) {
        uint16_t end = rd_u16(d, endCode + i * 2);
        if (codepoint > end) continue;
        uint16_t start = rd_u16(d, startCode + i * 2);
        if (codepoint < start) return 0;         /* in a gap */
        int16_t delta = rd_i16(d, idDelta + i * 2);
        uint16_t ro = rd_u16(d, idRange + i * 2);
        if (ro == 0) return (uint16_t)(codepoint + delta);
        /* indirect: &glyphIdArray[...] = idRangeOffset addr + ro + 2*(cp-start) */
        uint32_t addr = idRange + i * 2 + ro + (uint32_t)(codepoint - start) * 2;
        if (addr + 2 > f->data_len) return 0;
        uint16_t g = rd_u16(d, addr);
        if (g == 0) return 0;
        return (uint16_t)(g + delta);
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Section 3: glyf/loca outline extraction                                   */
/* ------------------------------------------------------------------------- */

static int loca_range(struct font *f, uint32_t gi, uint32_t *g_off, uint32_t *g_end) {
    const uint8_t *d = f->data;
    if (gi + 1 > f->num_glyphs) return 0;
    if (f->index_to_loc_fmt == 0) {
        uint32_t a = f->loca_offset + gi * 2;
        if (a + 4 > f->data_len) return 0;
        *g_off = (uint32_t)rd_u16(d, a) * 2;
        *g_end = (uint32_t)rd_u16(d, a + 2) * 2;
    } else {
        uint32_t a = f->loca_offset + gi * 4;
        if (a + 8 > f->data_len) return 0;
        *g_off = rd_u32(d, a);
        *g_end = rd_u32(d, a + 4);
    }
    return 1;
}

static uint16_t glyph_advance(struct font *f, uint32_t gi) {
    uint32_t n = f->num_h_metrics;
    if (n == 0) return 0;
    uint32_t idx = gi < n ? gi : n - 1;
    uint32_t a = f->hmtx_offset + idx * 4;
    if (a + 2 > f->data_len) return 0;
    return rd_u16(f->data, a);
}

/* TrueType simple-glyph flag bits */
#define GF_ON_CURVE  0x01
#define GF_X_SHORT   0x02
#define GF_Y_SHORT   0x04
#define GF_REPEAT    0x08
#define GF_X_SAME    0x10   /* if X_SHORT: x is positive; else x delta is 0 */
#define GF_Y_SAME    0x20

static bool parse_simple_glyph(struct font *f, uint32_t goff, int16_t ncont,
                               struct glyph_outline *out) {
    const uint8_t *d = f->data;
    uint32_t p = goff + 10;
    if (p + (uint32_t)ncont * 2 + 2 > f->data_len) return false;

    uint16_t *ends = (uint16_t *)malloc(sizeof(uint16_t) * ncont);
    if (!ends) return false;
    for (int i = 0; i < ncont; i++) { ends[i] = rd_u16(d, p); p += 2; }
    uint32_t n_points = (uint32_t)ends[ncont - 1] + 1;

    uint16_t instr_len = rd_u16(d, p); p += 2;
    p += instr_len;                             /* skip hinting bytecode (Section 0) */
    if (p > f->data_len) { free(ends); return false; }

    uint8_t *flags = (uint8_t *)malloc(n_points);
    if (!flags) { free(ends); return false; }
    for (uint32_t i = 0; i < n_points; ) {
        if (p >= f->data_len) { free(ends); free(flags); return false; }
        uint8_t fl = d[p++];
        flags[i++] = fl;
        if (fl & GF_REPEAT) {
            if (p >= f->data_len) { free(ends); free(flags); return false; }
            uint8_t rep = d[p++];
            while (rep-- && i < n_points) flags[i++] = fl;
        }
    }

    float *xs = (float *)malloc(sizeof(float) * n_points);
    float *ys = (float *)malloc(sizeof(float) * n_points);
    if (!xs || !ys) { free(ends); free(flags); free(xs); free(ys); return false; }

    int32_t x = 0;
    for (uint32_t i = 0; i < n_points; i++) {
        uint8_t fl = flags[i];
        if (fl & GF_X_SHORT) {
            if (p >= f->data_len) break;
            uint8_t dx = d[p++];
            x += (fl & GF_X_SAME) ? (int32_t)dx : -(int32_t)dx;
        } else if (!(fl & GF_X_SAME)) {
            if (p + 2 > f->data_len) break;
            x += rd_i16(d, p); p += 2;
        }
        xs[i] = (float)x;
    }
    int32_t y = 0;
    for (uint32_t i = 0; i < n_points; i++) {
        uint8_t fl = flags[i];
        if (fl & GF_Y_SHORT) {
            if (p >= f->data_len) break;
            uint8_t dy = d[p++];
            y += (fl & GF_Y_SAME) ? (int32_t)dy : -(int32_t)dy;
        } else if (!(fl & GF_Y_SAME)) {
            if (p + 2 > f->data_len) break;
            y += rd_i16(d, p); p += 2;
        }
        ys[i] = (float)y;
    }

    out->n_contours = (uint32_t)ncont;
    out->contours = (struct glyph_contour *)calloc(ncont, sizeof(struct glyph_contour));
    if (!out->contours) { free(ends); free(flags); free(xs); free(ys); return false; }
    uint32_t start = 0;
    for (int c = 0; c < ncont; c++) {
        uint32_t cend = ends[c];
        uint32_t cnt = cend - start + 1;
        struct glyph_contour *ct = &out->contours[c];
        ct->points = (struct glyph_point *)malloc(sizeof(struct glyph_point) * cnt);
        ct->n_points = cnt;
        for (uint32_t k = 0; k < cnt; k++) {
            ct->points[k].x = xs[start + k];
            ct->points[k].y = ys[start + k];
            ct->points[k].on_curve = (flags[start + k] & GF_ON_CURVE) != 0;
        }
        start = cend + 1;
    }

    free(ends); free(flags); free(xs); free(ys);
    return true;
}

/* composite-glyph flag bits */
#define CG_ARG_WORDS     0x0001
#define CG_ARGS_XY       0x0002
#define CG_HAVE_SCALE    0x0008
#define CG_MORE          0x0020
#define CG_XY_SCALE      0x0040
#define CG_2X2           0x0080

static bool parse_glyph_recursive(struct font *f, uint32_t gi, struct glyph_outline *out, int depth);

static bool parse_composite_glyph(struct font *f, uint32_t goff, struct glyph_outline *out, int depth) {
    const uint8_t *d = f->data;
    uint32_t p = goff + 10;
    /* accumulate components' contours into a growing array */
    out->n_contours = 0; out->contours = 0;

    for (;;) {
        if (p + 4 > f->data_len) return false;
        uint16_t flags = rd_u16(d, p); p += 2;
        uint16_t comp_gi = rd_u16(d, p); p += 2;

        float dx, dy;
        if (flags & CG_ARG_WORDS) {
            if (p + 4 > f->data_len) return false;
            dx = (float)rd_i16(d, p); dy = (float)rd_i16(d, p + 2); p += 4;
        } else {
            if (p + 2 > f->data_len) return false;
            dx = (float)(int8_t)d[p]; dy = (float)(int8_t)d[p + 1]; p += 2;
        }
        float sx = 1.0f, sy = 1.0f;
        if (flags & CG_HAVE_SCALE) {
            if (p + 2 > f->data_len) return false;
            sx = sy = rd_i16(d, p) / 16384.0f; p += 2;
        } else if (flags & CG_XY_SCALE) {
            if (p + 4 > f->data_len) return false;
            sx = rd_i16(d, p) / 16384.0f; sy = rd_i16(d, p + 2) / 16384.0f; p += 4;
        } else if (flags & CG_2X2) {
            if (p + 8 > f->data_len) return false;
            sx = rd_i16(d, p) / 16384.0f; sy = rd_i16(d, p + 6) / 16384.0f; p += 8; /* diag only (Section 0) */
        }

        struct glyph_outline comp;
        memset(&comp, 0, sizeof(comp));
        if (parse_glyph_recursive(f, comp_gi, &comp, depth + 1) && comp.n_contours > 0) {
            uint32_t base = out->n_contours;
            uint32_t total = base + comp.n_contours;
            struct glyph_contour *nc = (struct glyph_contour *)realloc(out->contours,
                                       sizeof(struct glyph_contour) * total);
            if (nc) {
                out->contours = nc;
                for (uint32_t c = 0; c < comp.n_contours; c++) {
                    struct glyph_contour *src = &comp.contours[c];
                    struct glyph_contour *dstc = &out->contours[base + c];
                    dstc->n_points = src->n_points;
                    dstc->points = (struct glyph_point *)malloc(sizeof(struct glyph_point) * src->n_points);
                    for (uint32_t k = 0; k < src->n_points; k++) {
                        dstc->points[k].x = src->points[k].x * sx + dx;
                        dstc->points[k].y = src->points[k].y * sy + dy;
                        dstc->points[k].on_curve = src->points[k].on_curve;
                    }
                }
                out->n_contours = total;
            }
        }
        font_free_outline(&comp);
        if (!(flags & CG_MORE)) break;
    }
    return true;
}

static bool parse_glyph_recursive(struct font *f, uint32_t gi, struct glyph_outline *out, int depth) {
    if (depth > 5) return false;   /* guard against pathological composite cycles */
    memset(out, 0, sizeof(*out));
    uint32_t goff, gend;
    if (!loca_range(f, gi, &goff, &gend)) return false;
    out->advance_width = glyph_advance(f, gi);
    if (gend <= goff) return true;   /* empty glyph (e.g. space) -- valid, 0 contours */

    uint32_t abs_off = f->glyf_offset + goff;
    if (abs_off + 10 > f->data_len) return false;
    int16_t ncont = rd_i16(f->data, abs_off);
    out->x_min = rd_i16(f->data, abs_off + 2);
    out->y_min = rd_i16(f->data, abs_off + 4);
    out->x_max = rd_i16(f->data, abs_off + 6);
    out->y_max = rd_i16(f->data, abs_off + 8);

    if (ncont >= 0) return parse_simple_glyph(f, abs_off, ncont, out);
    return parse_composite_glyph(f, abs_off, out, depth);
}

bool font_get_glyph_outline(struct font *f, uint32_t glyph_index, struct glyph_outline *out) {
    if (!f || !out) return false;
    return parse_glyph_recursive(f, glyph_index, out, 0);
}

void font_free_outline(struct glyph_outline *o) {
    if (!o || !o->contours) return;
    for (uint32_t i = 0; i < o->n_contours; i++) free(o->contours[i].points);
    free(o->contours);
    o->contours = 0; o->n_contours = 0;
}

/* ------------------------------------------------------------------------- */
/* Section 4: rasterization (supersample scanline fill + box downsample)     */
/* ------------------------------------------------------------------------- */

#define SS 4   /* linear supersample factor */

static uint32_t g_rasterize_calls;
uint32_t font_debug_rasterize_count(void) { return g_rasterize_calls; }

struct fpt { float x, y; };

/* Flatten one contour (font-unit points) into a closed pixel-space, y-down
 * polyline, reconstructing implicit on-curve midpoints (Section 3). Appends to
 * *poly. Algorithm: rotate the ring to begin on an on-curve anchor (synthesize
 * one if the contour is all off-curve), then walk emitting a straight segment
 * to each on-curve point and a 12-step quadratic Bezier through each control
 * point, materializing the implicit midpoint when two controls are adjacent. */
static void flatten_contour(const struct glyph_point *pts, uint32_t n,
                            float scale, struct fpt **poly, int *np, int *cap) {
    if (n == 0) return;

    /* build a rotated working ring E[] that STARTS on an on-curve point */
    struct fpt *E = (struct fpt *)malloc(sizeof(struct fpt) * (n + 1));
    bool *on = (bool *)malloc(sizeof(bool) * (n + 1));
    uint32_t m = 0;
    int s = -1;
    for (uint32_t i = 0; i < n; i++) if (pts[i].on_curve) { s = (int)i; break; }
    if (s >= 0) {
        for (uint32_t k = 0; k < n; k++) {
            uint32_t i = (uint32_t)s + k; if (i >= n) i -= n;
            E[m].x = pts[i].x * scale; E[m].y = -pts[i].y * scale; on[m] = pts[i].on_curve; m++;
        }
    } else {
        /* no on-curve: synthesize an anchor at the mid of last & first */
        E[m].x = (pts[n-1].x + pts[0].x) * 0.5f * scale;
        E[m].y = -(pts[n-1].y + pts[0].y) * 0.5f * scale; on[m] = true; m++;
        for (uint32_t k = 0; k < n; k++) {
            E[m].x = pts[k].x * scale; E[m].y = -pts[k].y * scale; on[m] = false; m++;
        }
    }

    #define PUSH(px,py) do { \
        if (*np >= *cap) { *cap = *cap ? *cap*2 : 64; *poly = (struct fpt*)realloc(*poly, sizeof(struct fpt)*(*cap)); } \
        (*poly)[*np].x = (px); (*poly)[*np].y = (py); (*np)++; } while (0)

    struct fpt anchor = E[0], cur = anchor;
    PUSH(anchor.x, anchor.y);
    uint32_t j = 1;
    while (j <= m) {
        struct fpt p = (j < m) ? E[j] : anchor;   /* j==m closes back to the anchor */
        int p_on = (j < m) ? on[j] : 1;
        if (p_on) {
            PUSH(p.x, p.y);                        /* straight segment */
            cur = p; j++;
        } else {
            struct fpt ctrl = p, end;
            uint32_t nj = j + 1;
            struct fpt np2 = (nj < m) ? E[nj] : anchor;
            int n_on = (nj < m) ? on[nj] : 1;
            if (n_on) { end = np2; }
            else { end.x = (ctrl.x + np2.x) * 0.5f; end.y = (ctrl.y + np2.y) * 0.5f; }
            for (int st = 1; st <= 12; st++) {     /* fixed 12-step quadratic */
                float t = st / 12.0f, u = 1.0f - t;
                float bx = u*u*cur.x + 2*u*t*ctrl.x + t*t*end.x;
                float by = u*u*cur.y + 2*u*t*ctrl.y + t*t*end.y;
                PUSH(bx, by);
            }
            cur = end;
            j += n_on ? 2 : 1;                     /* consumed the explicit end too */
        }
    }
    #undef PUSH
    free(E); free(on);
}

int font_debug_flatten_contour0(struct font *f, uint32_t gi, float scale,
                                float *xs, float *ys, int max) {
    struct glyph_outline o;
    if (!font_get_glyph_outline(f, gi, &o) || o.n_contours == 0) { font_free_outline(&o); return 0; }
    struct fpt *poly = 0; int np = 0, cap = 0;
    flatten_contour(o.contours[0].points, o.contours[0].n_points, scale, &poly, &np, &cap);
    int cnt = np < max ? np : max;
    for (int i = 0; i < cnt; i++) { xs[i] = poly[i].x; ys[i] = poly[i].y; }
    free(poly); font_free_outline(&o);
    return np;
}

bool font_rasterize_glyph(struct font *f, uint32_t glyph_index, float size_px,
                          uint8_t **out_coverage, int *out_w, int *out_h,
                          int *out_bearing_x, int *out_bearing_y, float *out_advance_px) {
    g_rasterize_calls++;
    struct glyph_outline o;
    if (!font_get_glyph_outline(f, glyph_index, &o)) return false;

    float scale = size_px / (float)f->units_per_em;
    *out_advance_px = o.advance_width * scale;

    /* flatten every contour into one polyline list, tracking contour spans */
    uint32_t ncont = o.n_contours;   /* SAVE before free -- the scanline loop
                                      * below still needs the count, and
                                      * font_free_outline zeroes o.n_contours */
    struct fpt *poly = 0; int np = 0, cap = 0;
    int *span_end = (int *)malloc(sizeof(int) * (ncont ? ncont : 1));
    for (uint32_t c = 0; c < ncont; c++) {
        flatten_contour(o.contours[c].points, o.contours[c].n_points, scale, &poly, &np, &cap);
        span_end[c] = np;   /* this contour's points are [prev_end, np) */
    }
    font_free_outline(&o);

    if (np == 0) {   /* blank glyph (space): valid, empty bitmap */
        free(poly); free(span_end);
        *out_coverage = 0; *out_w = 0; *out_h = 0; *out_bearing_x = 0; *out_bearing_y = 0;
        return true;
    }

    float minx = 1e30f, miny = 1e30f, maxx = -1e30f, maxy = -1e30f;
    for (int i = 0; i < np; i++) {
        if (poly[i].x < minx) minx = poly[i].x;
        if (poly[i].x > maxx) maxx = poly[i].x;
        if (poly[i].y < miny) miny = poly[i].y;
        if (poly[i].y > maxy) maxy = poly[i].y;
    }
    int ox = (int)floorf(minx), oy = (int)floorf(miny);
    int w = (int)ceilf(maxx) - ox;
    int h = (int)ceilf(maxy) - oy;
    if (w <= 0) w = 1;
    if (h <= 0) h = 1;

    int sw = w * SS, sh = h * SS;
    uint8_t *ss = (uint8_t *)calloc((size_t)sw * sh, 1);
    if (!ss) { free(poly); free(span_end); return false; }

    /* scanline fill, nonzero winding, at supersample resolution */
    for (int sy = 0; sy < sh; sy++) {
        float yc = ((float)sy + 0.5f) / SS + oy;   /* back to pixel space */
        /* gather edge crossings across ALL contours (joint fill for holes) */
        float xs[256]; int dir[256]; int nx = 0;
        int cstart = 0;
        for (uint32_t c = 0; c < ncont; c++) {
            int cend = span_end[c];
            for (int i = cstart; i < cend; i++) {
                int j = (i + 1 < cend) ? i + 1 : cstart;   /* close the contour */
                float y0 = poly[i].y, y1 = poly[j].y, x0 = poly[i].x, x1 = poly[j].x;
                if (y0 == y1) continue;
                if ((yc >= y0 && yc < y1) || (yc >= y1 && yc < y0)) {
                    float t = (yc - y0) / (y1 - y0);
                    float xc = x0 + t * (x1 - x0);
                    if (nx < 256) { xs[nx] = xc; dir[nx] = (y1 > y0) ? 1 : -1; nx++; }
                }
            }
            cstart = cend;
        }
        /* sort crossings by x (insertion sort; nx is tiny) */
        for (int a = 1; a < nx; a++) {
            float kx = xs[a]; int kd = dir[a]; int b = a - 1;
            while (b >= 0 && xs[b] > kx) { xs[b+1] = xs[b]; dir[b+1] = dir[b]; b--; }
            xs[b+1] = kx; dir[b+1] = kd;
        }
        int wind = 0;
        for (int a = 0; a < nx - 1; a++) {
            wind += dir[a];
            if (wind != 0) {
                float xa = (xs[a]   - ox) * SS;
                float xb = (xs[a+1] - ox) * SS;
                int ia = (int)(xa + 0.5f), ib = (int)(xb + 0.5f);
                if (ia < 0) ia = 0;
                if (ib > sw) ib = sw;
                for (int px = ia; px < ib; px++) ss[sy * sw + px] = 1;
            }
        }
    }

    /* box-downsample SSxSS -> one coverage byte (this is the AA) */
    uint8_t *cov = (uint8_t *)malloc((size_t)w * h);
    for (int py = 0; py < h; py++) {
        for (int px = 0; px < w; px++) {
            int sum = 0;
            for (int dy = 0; dy < SS; dy++)
                for (int dx = 0; dx < SS; dx++)
                    sum += ss[(py*SS+dy)*sw + (px*SS+dx)];
            cov[py * w + px] = (uint8_t)((sum * 255) / (SS * SS));
        }
    }

    free(ss); free(poly); free(span_end);
    *out_coverage = cov; *out_w = w; *out_h = h;
    *out_bearing_x = ox;
    *out_bearing_y = -oy;   /* pixels from baseline up to glyph top (oy is negative, y-down) */
    return true;
}

/* ------------------------------------------------------------------------- */
/* Section 5: glyph atlas                                                     */
/* ------------------------------------------------------------------------- */

static struct glyph_atlas g_atlas;
struct glyph_atlas *font_global_atlas(void) { return &g_atlas; }

static void atlas_reset(struct glyph_atlas *a) {
    a->shelf_x = a->shelf_y = a->shelf_row_h = 0;
    a->n_entries = 0;
    for (uint32_t i = 0; i < GLYPH_CACHE_MAX; i++) a->entries[i].used = false;
}

struct glyph_cache_entry *glyph_cache_lookup_or_rasterize(
    struct glyph_atlas *atlas, struct font *f, uint32_t codepoint, float size_px) {
    if (!atlas || !f) return 0;
    uint16_t sx4 = (uint16_t)(size_px * 4.0f + 0.5f);
    /* find this font's handle (for the cache key) */
    uint32_t fh = 0;
    for (int i = 0; i < MAX_FONTS; i++) if (&g_fonts[i] == f) { fh = (uint32_t)i + 1; break; }

    for (uint32_t i = 0; i < atlas->n_entries; i++) {
        struct glyph_cache_entry *e = &atlas->entries[i];
        if (e->used && e->font_handle == fh && e->codepoint == codepoint && e->size_px_x4 == sx4)
            return e;   /* cache hit -- no rasterize */
    }

    /* miss: rasterize */
    uint32_t gi = font_codepoint_to_glyph(f, codepoint);   /* 0 == .notdef, still rendered */
    uint8_t *cov = 0; int w = 0, h = 0, bx = 0, by = 0; float adv = 0;
    if (!font_rasterize_glyph(f, gi, size_px, &cov, &w, &h, &bx, &by, &adv)) return 0;

    /* pack into the shelf; clear-and-restart on exhaustion (Section 5) */
    if (w > 0 && h > 0) {
        if (atlas->shelf_x + (uint32_t)w > GLYPH_ATLAS_SIZE) {
            atlas->shelf_x = 0;
            atlas->shelf_y += atlas->shelf_row_h;
            atlas->shelf_row_h = 0;
        }
        if (atlas->shelf_y + (uint32_t)h > GLYPH_ATLAS_SIZE || atlas->n_entries >= GLYPH_CACHE_MAX) {
            atlas_reset(atlas);
        }
    } else if (atlas->n_entries >= GLYPH_CACHE_MAX) {
        atlas_reset(atlas);
    }

    struct glyph_cache_entry *e = &atlas->entries[atlas->n_entries++];
    e->used = true; e->font_handle = fh; e->codepoint = codepoint; e->size_px_x4 = sx4;
    e->bearing_x = (int16_t)bx; e->bearing_y = (int16_t)by; e->advance_px = adv;
    e->atlas_w = (uint16_t)(w > 0 ? w : 0); e->atlas_h = (uint16_t)(h > 0 ? h : 0);

    if (cov && w > 0 && h > 0) {
        e->atlas_x = (uint16_t)atlas->shelf_x;
        e->atlas_y = (uint16_t)atlas->shelf_y;
        for (int yy = 0; yy < h; yy++)
            for (int xx = 0; xx < w; xx++)
                atlas->coverage[atlas->shelf_y + yy][atlas->shelf_x + xx] = cov[yy * w + xx];
        atlas->shelf_x += (uint32_t)w;
        if ((uint32_t)h > atlas->shelf_row_h) atlas->shelf_row_h = (uint32_t)h;
    } else {
        e->atlas_x = e->atlas_y = 0;   /* blank glyph (space): no pixels, real advance */
    }
    free(cov);
    return e;
}

/* ------------------------------------------------------------------------- */
/* Section 6: draw_text                                                       */
/* ------------------------------------------------------------------------- */

/* Standard 1-4 byte UTF-8 decode; returns bytes consumed (>=1). */
static int utf8_decode(const char *s, uint32_t *cp) {
    const uint8_t *u = (const uint8_t *)s;
    if (u[0] < 0x80) { *cp = u[0]; return 1; }
    if ((u[0] & 0xE0) == 0xC0 && (u[1] & 0xC0) == 0x80) {
        *cp = ((uint32_t)(u[0] & 0x1F) << 6) | (u[1] & 0x3F); return 2;
    }
    if ((u[0] & 0xF0) == 0xE0 && (u[1] & 0xC0) == 0x80 && (u[2] & 0xC0) == 0x80) {
        *cp = ((uint32_t)(u[0] & 0x0F) << 12) | ((uint32_t)(u[1] & 0x3F) << 6) | (u[2] & 0x3F); return 3;
    }
    if ((u[0] & 0xF8) == 0xF0 && (u[1] & 0xC0) == 0x80 && (u[2] & 0xC0) == 0x80 && (u[3] & 0xC0) == 0x80) {
        *cp = ((uint32_t)(u[0] & 0x07) << 18) | ((uint32_t)(u[1] & 0x3F) << 12)
            | ((uint32_t)(u[2] & 0x3F) << 6) | (u[3] & 0x3F); return 4;
    }
    *cp = 0xFFFD; return 1;   /* invalid byte -> replacement, advance one */
}

/* Gamma (approx 2.0) blend tables, built once. The glyph blend mixes in LINEAR
 * light: out = sqrt(src^2*eff + dst^2*inv). Done literally that was 3 dst
 * squarings + 3 sqrtf PER TEXT PIXEL -- and in the freestanding build sqrtf is
 * an 8-iteration Newton loop (24 float divides/pixel just to de-gamma). Text
 * covers most of the screen, so this dominated glyph cost under TCG. Instead:
 *   G_LIN[d]   = (d/255)^2                    -- linearise a dst byte (no square)
 *   G_DELIN[k] = round(sqrt(k/4096)*255)      -- de-linearise (no sqrt)
 * The blend arg is a convex mix of two [0,1] values so it stays in [0,1]; the
 * 12-bit de-gamma table quantises to <=1/255, visually identical. */
#define G_DELIN_BITS 12
#define G_DELIN_N    (1 << G_DELIN_BITS)   /* 4096 */
static float   g_lin[256];
static uint8_t g_delin[G_DELIN_N + 1];
static int     g_gamma_ready;
static void gamma_tables_init(void) {
    if (g_gamma_ready) return;
    for (int d = 0; d < 256; d++) { float x = d / 255.0f; g_lin[d] = x * x; }
    for (int k = 0; k <= G_DELIN_N; k++) {
        int v = (int)(sqrtf((float)k / (float)G_DELIN_N) * 255.0f + 0.5f);
        g_delin[k] = (uint8_t)(v > 255 ? 255 : v);
    }
    g_gamma_ready = 1;
}

/* premultiplied-BGRA pixel over-blend, gated by the backend clip/dirty. */
static void blit_coverage_tinted(struct render_target *rt, int dx, int dy,
                                 struct glyph_atlas *atlas, int ax, int ay, int aw, int ah,
                                 struct color color, float opacity) {
    if (!g_gamma_ready) gamma_tables_init();
    /* src linear-light channels are constant for the whole glyph */
    float sr2 = color.r * color.r, sg2 = color.g * color.g, sb2 = color.b * color.b;
    for (int yy = 0; yy < ah; yy++) {
        int py = dy + yy;
        if (py < 0 || py >= (int)rt->height) continue;
        uint32_t *row = (uint32_t *)((unsigned char *)rt->pixels + (size_t)py * rt->stride);
        for (int xx = 0; xx < aw; xx++) {
            int px = dx + xx;
            if (px < 0 || px >= (int)rt->width) continue;
            float cov = atlas->coverage[ay + yy][ax + xx] / 255.0f;
            float eff = color.a * cov * opacity * cpu_coverage_at(px + 0.5f, py + 0.5f);
            if (eff <= 0.0f) continue;
            uint32_t d = row[px];
            int db = d & 255, dg = (d>>8)&255, dr = (d>>16)&255, da = (d>>24)&255;
            float inv = 1.0f - eff;
            /* linear-light blend via tables: linearise dst (g_lin), mix, then
             * de-linearise (g_delin) -- no per-pixel square or sqrt. */
            float ar = sr2*eff + g_lin[dr]*inv;
            float ag = sg2*eff + g_lin[dg]*inv;
            float ab = sb2*eff + g_lin[db]*inv;
            int kr = (int)(ar * (float)G_DELIN_N); if (kr > G_DELIN_N) kr = G_DELIN_N; if (kr < 0) kr = 0;
            int kg = (int)(ag * (float)G_DELIN_N); if (kg > G_DELIN_N) kg = G_DELIN_N; if (kg < 0) kg = 0;
            int kb = (int)(ab * (float)G_DELIN_N); if (kb > G_DELIN_N) kb = G_DELIN_N; if (kb < 0) kb = 0;
            int nr = g_delin[kr], ng = g_delin[kg], nb = g_delin[kb];
            int na = (int)(eff * 255.0f + da * inv + 0.5f);
            if (na>255) na=255;
            row[px] = ((uint32_t)na<<24)|((uint32_t)nr<<16)|((uint32_t)ng<<8)|(uint32_t)nb;
        }
    }
}

static void backend_draw_text_impl(struct render_target *rt, float x, float y, const char *utf8,
                                   uint32_t font_handle, float size_px, struct color color, float opacity) {
    struct font *f = font_for_handle(font_handle);
    if (!f || !utf8) return;
    /* `y` is the top of the text's line box (what layout hands us); the glyph
     * baseline sits ASCENT below that. Without this offset every glyph is drawn
     * up by ~ascent and floats above its box. */
    float ascent_px = (float)f->ascent * (size_px / (float)f->units_per_em);
    float pen_x = x, pen_y = y + ascent_px;
    const char *p = utf8;
    /* Bound the scan: a dangling/non-NUL-terminated utf8 pointer (a corrupt
     * scene-text node) would otherwise read off the end forever. Real UI strings
     * are far shorter than this cap. */
    for (int guard = 0; *p && guard < 8192; guard++) {
        uint32_t cp;
        p += utf8_decode(p, &cp);
        struct glyph_cache_entry *g = glyph_cache_lookup_or_rasterize(&g_atlas, f, cp, size_px);
        if (g) {
            if (g->atlas_w > 0 && g->atlas_h > 0)
                blit_coverage_tinted(rt, (int)(pen_x + g->bearing_x), (int)(pen_y - g->bearing_y),
                                     &g_atlas, g->atlas_x, g->atlas_y, g->atlas_w, g->atlas_h,
                                     color, opacity);
            pen_x += g->advance_px;
        }
    }
}

void font_install_backend(void) {
    cpu_backend_get()->draw_text = backend_draw_text_impl;
}
