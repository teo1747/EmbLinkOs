/* ui/layout/layout.c -- EmbLink UI Piece 5 (see layout.h).
 *
 * Phase 1 measures intrinsic sizes bottom-up; Phase 2 arranges top-down with
 * CSS-accurate flex grow/shrink, justify/align, and text wrapping, writing the
 * resolved parent-relative geometry straight into Piece 3's scene nodes. */

#include "layout.h"
#include <stdlib.h>

/* ------------------------------------------------------------------------- */
/* arena (same paged + generation ABA-guard discipline as Piece 3)           */
/* ------------------------------------------------------------------------- */

static void lz(void *p, size_t n) { unsigned char *b = p; for (size_t i = 0; i < n; i++) b[i] = 0; }

static struct layout_node *lslot(struct layout_arena *a, uint32_t idx) {
    uint32_t pg = idx / LAYOUT_PAGE_SIZE, off = idx % LAYOUT_PAGE_SIZE;
    if (pg >= LAYOUT_MAX_PAGES || !a->pages[pg]) return 0;
    return &a->pages[pg][off];
}
static bool lensure(struct layout_arena *a, uint32_t idx) {
    uint32_t pg = idx / LAYOUT_PAGE_SIZE;
    if (pg >= LAYOUT_MAX_PAGES) return false;
    if (a->pages[pg]) return true;
    struct layout_node *p = malloc(sizeof(struct layout_node) * LAYOUT_PAGE_SIZE);
    if (!p) return false;
    lz(p, sizeof(struct layout_node) * LAYOUT_PAGE_SIZE);
    a->pages[pg] = p; a->n_pages_allocated++;
    return true;
}
void layout_arena_init(struct layout_arena *a) { lz(a, sizeof(*a)); a->next_never_used = 1; }
void layout_arena_destroy(struct layout_arena *a) {
    for (uint32_t i = 0; i < LAYOUT_MAX_PAGES; i++) { free(a->pages[i]); a->pages[i] = 0; }
    a->n_pages_allocated = 0; a->free_list_head = 0; a->next_never_used = 1;
}

struct layout_node *layout_resolve(struct layout_arena *a, struct layout_handle h) {
    if (h.index == 0) return 0;
    struct layout_node *n = lslot(a, h.index);
    if (!n || n->self.index != h.index || n->self.generation != h.generation) return 0;
    return n;
}

static void lappend_child(struct layout_arena *a, struct layout_handle parent, struct layout_handle child);

static void lunlink_from_parent(struct layout_arena *a, struct layout_handle h) {
    struct layout_node *n = layout_resolve(a, h);
    if (!n) return;
    struct layout_node *p = layout_resolve(a, n->parent);
    if (!p) { n->next_sibling = LAYOUT_HANDLE_NULL; return; }
    if (p->first_child.index == h.index) { p->first_child = n->next_sibling; }
    else {
        struct layout_handle it = p->first_child;
        while (!layout_handle_is_null(it)) {
            struct layout_node *in = layout_resolve(a, it);
            if (!in) break;
            if (in->next_sibling.index == h.index) { in->next_sibling = n->next_sibling; break; }
            it = in->next_sibling;
        }
    }
    n->next_sibling = LAYOUT_HANDLE_NULL;
}

void layout_reparent(struct layout_arena *a, struct layout_handle h,
                     struct layout_handle new_parent, struct layout_handle after) {
    struct layout_node *n = layout_resolve(a, h);
    struct layout_node *np = layout_resolve(a, new_parent);
    if (!n || !np) return;
    lunlink_from_parent(a, h);
    n->parent = new_parent;
    if (layout_handle_is_null(after)) {
        n->next_sibling = np->first_child;
        np->first_child = h;
    } else {
        struct layout_node *as = layout_resolve(a, after);
        if (!as || after.index == h.index) {   /* bad/self anchor -> append last */
            n->next_sibling = LAYOUT_HANDLE_NULL; lappend_child(a, new_parent, h); return;
        }
        n->next_sibling = as->next_sibling;
        as->next_sibling = h;
        if (n->next_sibling.index == h.index)  /* never point at self */
            n->next_sibling = LAYOUT_HANDLE_NULL;
    }
}

static uint32_t lalloc_slot(struct layout_arena *a) {
    if (a->free_list_head) {
        uint32_t i = a->free_list_head;
        struct layout_node *n = lslot(a, i);
        a->free_list_head = n ? n->next_sibling.index : 0;
        return i;
    }
    uint32_t i = a->next_never_used;
    if (i / LAYOUT_PAGE_SIZE >= LAYOUT_MAX_PAGES || !lensure(a, i)) return 0;
    a->next_never_used++;
    return i;
}

static void lappend_child(struct layout_arena *a, struct layout_handle parent, struct layout_handle child) {
    struct layout_node *p = layout_resolve(a, parent);
    if (!p) return;
    if (layout_handle_is_null(p->first_child)) { p->first_child = child; return; }
    struct layout_handle c = p->first_child;
    for (;;) {
        struct layout_node *cn = layout_resolve(a, c);
        if (!cn || layout_handle_is_null(cn->next_sibling)) { if (cn) cn->next_sibling = child; return; }
        c = cn->next_sibling;
    }
}

struct layout_handle layout_create_node(struct layout_arena *a, struct layout_handle parent) {
    uint32_t idx = lalloc_slot(a);
    if (!idx) return LAYOUT_HANDLE_NULL;
    struct layout_node *n = lslot(a, idx);
    uint32_t gen = n->self.generation; if (gen == 0) gen = 1;
    lz(n, sizeof(*n));
    n->self.index = idx; n->self.generation = gen;
    n->width.mode = SIZE_INTRINSIC; n->height.mode = SIZE_INTRINSIC;
    struct layout_handle h = n->self;
    if (layout_resolve(a, parent)) { n->parent = parent; lappend_child(a, parent, h); }
    return h;
}

void layout_destroy_node(struct layout_arena *a, struct layout_handle h) {
    struct layout_node *n = layout_resolve(a, h);
    if (!n) return;
    struct layout_handle c = n->first_child;
    while (!layout_handle_is_null(c)) {
        struct layout_node *cn = layout_resolve(a, c);
        struct layout_handle next = cn ? cn->next_sibling : LAYOUT_HANDLE_NULL;
        layout_destroy_node(a, c);
        c = next;
    }
    n->self.index = 0; n->self.generation++;
    n->next_sibling.index = a->free_list_head; a->free_list_head = h.index;
}

/* ------------------------------------------------------------------------- */
/* font metric helpers                                                       */
/* ------------------------------------------------------------------------- */

static float text_char_advance(struct font *f, uint32_t cp, float size_px) {
    struct glyph_cache_entry *e = glyph_cache_lookup_or_rasterize(font_global_atlas(), f, cp, size_px);
    return e ? e->advance_px : 0.0f;
}
/* one-line, unwrapped pixel width of an ASCII string (Phase-1 text intrinsic). */
static float text_line_width(struct font *f, const char *s, float size_px) {
    float w = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) w += text_char_advance(f, *p, size_px);
    return w;
}
static float font_line_height(struct font *f, float size_px) {
    float scale = size_px / (float)f->units_per_em;
    return (float)(f->ascent + f->descent + f->line_gap) * scale;
}

/* resolve a layout node's paired scene node (for TEXT/IMAGE reads + write-back) */
static struct scene_node *paired(struct scene_arena *sa, struct layout_node *n) {
    return scene_resolve(sa, n->scene_node);
}
static int is_text(struct scene_arena *sa, struct layout_node *n) {
    struct scene_node *s = paired(sa, n);
    return s && s->kind == SCENE_NODE_TEXT;
}

/* ------------------------------------------------------------------------- */
/* text wrapping (Section 4)                                                 */
/* ------------------------------------------------------------------------- */

int layout_debug_wrap_lines(struct layout_arena *la, struct scene_arena *sa,
                            struct layout_handle text_node, float width) {
    struct layout_node *n = layout_resolve(la, text_node);
    if (!n) return 0;
    struct scene_node *s = paired(sa, n);
    if (!s || s->kind != SCENE_NODE_TEXT || !s->data.text.utf8) return 0;
    struct font *f = font_for_handle(s->data.text.font_handle);
    if (!f) return 0;
    float size = s->data.text.size_px;
    const char *str = s->data.text.utf8;
    float space_w = text_char_advance(f, ' ', size);

    int lines = 1; float cur = 0;
    const unsigned char *p = (const unsigned char *)str;
    while (*p) {
        if (*p == ' ') { p++; continue; }               /* word boundary */
        const unsigned char *w0 = p;
        while (*p && *p != ' ') p++;                     /* word = [w0, p) */
        float word_w = 0;
        for (const unsigned char *q = w0; q < p; q++) word_w += text_char_advance(f, *q, size);

        if (word_w <= width) {                           /* whole word fits on a line */
            if (cur == 0) cur = word_w;
            else if (cur + space_w + word_w <= width) cur += space_w + word_w;
            else { lines++; cur = word_w; }
        } else {                                         /* character-wrap fallback */
            if (cur > 0) { lines++; cur = 0; }
            for (const unsigned char *q = w0; q < p; q++) {
                float cw = text_char_advance(f, *q, size);
                if (cur == 0) cur = cw;
                else if (cur + cw <= width) cur += cw;
                else { lines++; cur = cw; }
            }
        }
    }
    return lines;
}

float layout_measure_height_at_width(struct layout_arena *la, struct scene_arena *sa,
                                     struct layout_handle text_node, float width) {
    struct layout_node *n = layout_resolve(la, text_node);
    if (!n) return 0;
    struct scene_node *s = paired(sa, n);
    if (!s || s->kind != SCENE_NODE_TEXT) return 0;
    struct font *f = font_for_handle(s->data.text.font_handle);
    if (!f) return 0;
    int lines = layout_debug_wrap_lines(la, sa, text_node, width);
    return lines * font_line_height(f, s->data.text.size_px);
}

/* ------------------------------------------------------------------------- */
/* Phase 1: intrinsic sizes (bottom-up)                                      */
/* ------------------------------------------------------------------------- */

static void measure_intrinsic(struct layout_arena *la, struct scene_arena *sa, struct layout_handle h) {
    struct layout_node *n = layout_resolve(la, h);
    if (!n) return;
    for (struct layout_handle c = n->first_child; !layout_handle_is_null(c); ) {
        struct layout_node *cn = layout_resolve(la, c);
        struct layout_handle next = cn ? cn->next_sibling : LAYOUT_HANDLE_NULL;
        measure_intrinsic(la, sa, c);
        c = next;
    }
    n = layout_resolve(la, h);   /* re-resolve (children calls may have grown pages) */

    /* --- intrinsic width --- */
    if (n->width.mode == SIZE_FIXED) {
        n->intrinsic_w = n->width.fixed_value;
    } else if (!n->is_container) {
        struct scene_node *s = paired(sa, n);
        if (s && s->kind == SCENE_NODE_TEXT && s->data.text.utf8) {
            struct font *f = font_for_handle(s->data.text.font_handle);
            n->intrinsic_w = f ? text_line_width(f, s->data.text.utf8, s->data.text.size_px) : 0;
        } else if (s && s->kind == SCENE_NODE_IMAGE) {
            n->intrinsic_w = (float)s->data.image.w;
        } else {
            n->intrinsic_w = 0;
        }
    } else {
        float sum = 0, mx = 0; int cnt = 0;
        for (struct layout_handle c = n->first_child; !layout_handle_is_null(c); ) {
            struct layout_node *cn = layout_resolve(la, c);
            if (!cn) break;
            if (cn->is_overlay) { c = cn->next_sibling; continue; }   /* out of flow */
            sum += cn->intrinsic_w; if (cn->intrinsic_w > mx) mx = cn->intrinsic_w; cnt++;
            c = cn->next_sibling;
        }
        if (n->axis == AXIS_ROW) n->intrinsic_w = sum + (cnt > 1 ? n->spacing * (cnt - 1) : 0);
        else                     n->intrinsic_w = mx;
        n->intrinsic_w += n->padding_left + n->padding_right;
    }

    /* --- intrinsic height (cross for ROW / main for COLUMN). TEXT height is
     * deferred to arrange (Section 2/3); use one line as a placeholder. --- */
    if (n->height.mode == SIZE_FIXED) {
        n->intrinsic_h = n->height.fixed_value;
    } else if (!n->is_container) {
        struct scene_node *s = paired(sa, n);
        if (s && s->kind == SCENE_NODE_TEXT) {
            struct font *f = font_for_handle(s->data.text.font_handle);
            n->intrinsic_h = f ? font_line_height(f, s->data.text.size_px) : 0;
        } else if (s && s->kind == SCENE_NODE_IMAGE) {
            n->intrinsic_h = (float)s->data.image.h;
        } else {
            n->intrinsic_h = 0;
        }
    } else {
        float sum = 0, mx = 0; int cnt = 0;
        for (struct layout_handle c = n->first_child; !layout_handle_is_null(c); ) {
            struct layout_node *cn = layout_resolve(la, c);
            if (!cn) break;
            if (cn->is_overlay) { c = cn->next_sibling; continue; }   /* out of flow */
            sum += cn->intrinsic_h; if (cn->intrinsic_h > mx) mx = cn->intrinsic_h; cnt++;
            c = cn->next_sibling;
        }
        if (n->axis == AXIS_COLUMN) n->intrinsic_h = sum + (cnt > 1 ? n->spacing * (cnt - 1) : 0);
        else                        n->intrinsic_h = mx;
        n->intrinsic_h += n->padding_top + n->padding_bottom;
    }
}

/* ------------------------------------------------------------------------- */
/* Phase 2: arrange (top-down)                                               */
/* ------------------------------------------------------------------------- */

static void write_scene(struct scene_arena *sa, struct layout_node *n) {
    scene_set_size(sa, n->scene_node, n->resolved_w, n->resolved_h);
    /* resolved position + the authored post-layout offset (transitions/slides) */
    scene_set_transform(sa, n->scene_node,
                        n->resolved_x + n->offset_x, n->resolved_y + n->offset_y, 0,
                        0, 0, 0, 1, 1, 1, 1);
}

static void arrange(struct layout_arena *la, struct scene_arena *sa,
                    struct layout_handle h, float W, float H) {
    struct layout_node *n = layout_resolve(la, h);
    if (!n) return;
    n->resolved_w = W; n->resolved_h = H;
    write_scene(sa, n);
    if (!n->is_container) return;

    int is_row = (n->axis == AXIS_ROW);
    float content_main  = is_row ? (W - n->padding_left - n->padding_right)
                                 : (H - n->padding_top - n->padding_bottom);
    float content_cross = is_row ? (H - n->padding_top - n->padding_bottom)
                                 : (W - n->padding_left - n->padding_right);
    float main_pad0  = is_row ? n->padding_left : n->padding_top;
    float cross_pad0 = is_row ? n->padding_top : n->padding_left;

    /* gather children into a small array */
    struct layout_handle kids[64]; int nk = 0;
    for (struct layout_handle c = n->first_child; !layout_handle_is_null(c) && nk < 64; ) {
        struct layout_node *cn = layout_resolve(la, c);
        if (!cn) break;
        kids[nk++] = c;
        c = cn->next_sibling;
    }
    if (nk == 0) return;

    float base[64], finalm[64], crossv[64];
    float sum_base = 0, sum_grow = 0;

    for (int i = 0; i < nk; i++) {
        struct layout_node *k = layout_resolve(la, kids[i]);
        base[i] = 0;
        if (k->is_overlay) continue;         /* out of flow: sized to the parent below */
        struct layout_size *ms = is_row ? &k->width  : &k->height;
        enum layout_align   ca = k->align;   /* child's own cross alignment override? no -- parent's align applies */
        (void)ca;
        int ktext = is_text(sa, k);

        if (ms->mode == SIZE_FIXED) {
            base[i] = ms->fixed_value;
        } else if (ktext && !is_row) {
            /* COLUMN: main size is HEIGHT -> wrap at the child's cross WIDTH */
            float cw = (n->align == ALIGN_STRETCH) ? content_cross : k->intrinsic_w;
            base[i] = layout_measure_height_at_width(la, sa, kids[i], cw);
        } else {
            base[i] = is_row ? k->intrinsic_w : k->intrinsic_h;   /* incl. text one-line width (ROW flex-basis) */
        }
        sum_base += base[i];
        sum_grow += ms->flex_grow;
    }

    float total_base = sum_base + (nk > 1 ? n->spacing * (nk - 1) : 0);
    float remaining = content_main - total_base;

    for (int i = 0; i < nk; i++) finalm[i] = base[i];
    if (remaining > 0 && sum_grow > 0) {
        for (int i = 0; i < nk; i++) {
            struct layout_node *k = layout_resolve(la, kids[i]);
            if (k->is_overlay) continue;
            struct layout_size *ms = is_row ? &k->width : &k->height;
            finalm[i] = base[i] + remaining * (ms->flex_grow / sum_grow);
        }
    } else if (remaining < 0) {
        float sum_w = 0;
        for (int i = 0; i < nk; i++) {
            struct layout_node *k = layout_resolve(la, kids[i]);
            struct layout_size *ms = is_row ? &k->width : &k->height;
            sum_w += ms->flex_shrink * base[i];   /* CSS: shrink weighted BY base size */
        }
        if (sum_w > 0) {
            for (int i = 0; i < nk; i++) {
                struct layout_node *k = layout_resolve(la, kids[i]);
                struct layout_size *ms = is_row ? &k->width : &k->height;
                float shrink = (-remaining) * ((ms->flex_shrink * base[i]) / sum_w);
                finalm[i] = base[i] - shrink;
                if (finalm[i] < ms->min_size) finalm[i] = ms->min_size;
            }
        }
    }

    /* cross size per child */
    for (int i = 0; i < nk; i++) {
        struct layout_node *k = layout_resolve(la, kids[i]);
        crossv[i] = 0;
        if (k->is_overlay) continue;
        struct layout_size *cs = is_row ? &k->height : &k->width;
        int ktext = is_text(sa, k);
        if (cs->mode == SIZE_FIXED) {
            /* a definite cross size always wins -- CSS stretch only applies to
             * auto-sized items (a 50px-wide child of a stretch column stays
             * 50px; stretching it broke fixed-size boxes once the ROOT became
             * a stretch column). */
            crossv[i] = cs->fixed_value;
        } else if (n->align == ALIGN_STRETCH) {
            crossv[i] = content_cross;
        } else if (ktext && is_row) {
            crossv[i] = layout_measure_height_at_width(la, sa, kids[i], finalm[i]); /* height at final width */
        } else {
            crossv[i] = is_row ? k->intrinsic_h : k->intrinsic_w;
        }
    }

    /* main-axis positions per justify */
    float used = 0; for (int i = 0; i < nk; i++) used += finalm[i];
    float leftover = content_main - used - (nk > 1 ? n->spacing * (nk - 1) : 0);
    if (leftover < 0) leftover = 0;
    float cursor = main_pad0, gap = n->spacing;
    switch (n->justify) {
        case JUSTIFY_START:         break;
        case JUSTIFY_CENTER:        cursor += leftover * 0.5f; break;
        case JUSTIFY_END:           cursor += leftover; break;
        case JUSTIFY_SPACE_BETWEEN: if (nk > 1) gap = n->spacing + leftover / (nk - 1); break;
    }
    if (!is_row) cursor -= n->scroll_offset;   /* vertical scroll shifts children up */

    for (int i = 0; i < nk; i++) {
        struct layout_node *k = layout_resolve(la, kids[i]);
        if (k->is_overlay) {
            /* fill the parent's content box, independent of flow + cursor */
            k->resolved_x = n->padding_left; k->resolved_y = n->padding_top;
            k->resolved_w = W - n->padding_left - n->padding_right;
            k->resolved_h = H - n->padding_top - n->padding_bottom;
            if (k->is_container) arrange(la, sa, kids[i], k->resolved_w, k->resolved_h);
            else                 write_scene(sa, k);
            continue;   /* no cursor advance */
        }
        float main_pos = cursor;
        float cross_pos = cross_pad0;
        switch (n->align) {
            case ALIGN_START:   cross_pos = cross_pad0; break;
            case ALIGN_CENTER:  cross_pos = cross_pad0 + (content_cross - crossv[i]) * 0.5f; break;
            case ALIGN_END:     cross_pos = cross_pad0 + (content_cross - crossv[i]); break;
            case ALIGN_STRETCH: cross_pos = cross_pad0; break;
        }
        if (is_row) {
            k->resolved_x = main_pos; k->resolved_y = cross_pos;
            k->resolved_w = finalm[i]; k->resolved_h = crossv[i];
        } else {
            k->resolved_y = main_pos; k->resolved_x = cross_pos;
            k->resolved_h = finalm[i]; k->resolved_w = crossv[i];
        }
        if (k->is_container) arrange(la, sa, kids[i], k->resolved_w, k->resolved_h);
        else                 write_scene(sa, k);
        cursor += finalm[i] + gap;
    }
}

void layout_run(struct layout_arena *la, struct scene_arena *sa,
                struct layout_handle root, float W, float H) {
    struct layout_node *r = layout_resolve(la, root);
    if (!r) return;
    measure_intrinsic(la, sa, root);
    r = layout_resolve(la, root);
    r->resolved_x = 0; r->resolved_y = 0;
    arrange(la, sa, root, W, H);
}
