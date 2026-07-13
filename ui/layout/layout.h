#ifndef __EMBLINK_UI_LAYOUT_H__
#define __EMBLINK_UI_LAYOUT_H__

/* ui/layout/layout.h -- EmbLink UI Piece 5: the layout engine.
 *
 * Flexbox-style top-down proposed-size / bottom-up actual-size negotiation --
 * the model SwiftUI itself is under its declarative syntax, not a constraint
 * solver. Pure userland C, no kernel. Consumes Piece 4b font metrics for text
 * measurement; writes resolved geometry into Piece 3's scene tree via its
 * existing scene_set_size / scene_set_transform mutation API.
 *
 * A SEPARATE tree from the scene tree, 1:1 paired: each layout node references
 * exactly one scene node it produces output for. Same paged-arena +
 * {index,generation} ABA-safe handle discipline as Piece 3, reused.
 *
 * This piece is a PURE, fully-re-runnable function (input tree -> output
 * geometry). Deciding WHEN to re-run (dirty-subtree tracking) is Piece 6's job
 * -- the same boundary Piece 3 drew against Piece 4a's dirty-rect logic. */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "scene.h"   /* struct node_handle, struct scene_arena */
#include "font.h"    /* text measurement */

struct layout_handle { uint32_t index, generation; };
static const struct layout_handle LAYOUT_HANDLE_NULL = { 0, 0 };
static inline bool layout_handle_is_null(struct layout_handle h) { return h.index == 0; }

enum layout_axis    { AXIS_ROW, AXIS_COLUMN };
enum layout_justify { JUSTIFY_START, JUSTIFY_CENTER, JUSTIFY_END, JUSTIFY_SPACE_BETWEEN };
enum layout_align   { ALIGN_START, ALIGN_CENTER, ALIGN_END, ALIGN_STRETCH };
enum size_mode      { SIZE_FIXED, SIZE_INTRINSIC, SIZE_FLEX };

struct layout_size {
    enum size_mode mode;
    float fixed_value;    /* SIZE_FIXED only */
    float flex_grow;      /* weight for POSITIVE remaining space */
    float flex_shrink;    /* weight for NEGATIVE remaining space -- ORTHOGONAL to mode */
    float min_size;        /* floor a shrinking node won't cross; default 0 */
};

struct layout_node {
    struct layout_handle self;
    struct layout_handle parent;
    struct layout_handle first_child, next_sibling;   /* intrusive list (also free-list link) */

    struct node_handle scene_node;   /* the paired Piece-3 node this outputs to */

    bool is_container;                /* true: arranges children; false: leaf */
    enum layout_axis    axis;
    enum layout_justify justify;
    enum layout_align   align;

    float padding_top, padding_right, padding_bottom, padding_left;
    float spacing;                     /* main-axis gap between children */
    float scroll_offset;               /* column scroll: children shift up by this many px */
    float offset_x, offset_y;          /* post-layout translate ADDED to the resolved
                                        * position (CSS `transform: translate`-style) --
                                        * for transitions/slides; doesn't affect flow */
    bool  is_overlay;                  /* fill the parent, excluded from flow (modals/popovers) */

    struct layout_size width, height;  /* own sizing, per axis */

    /* --- computed scratch (not authored) --- */
    float intrinsic_w, intrinsic_h;                        /* Phase 1 */
    float resolved_x, resolved_y, resolved_w, resolved_h;  /* Phase 2, PARENT-relative px */

    bool dirty;   /* set by authoring mutation; consumed by Piece 6 */
};

/* ------------------------------------------------------------------------- */
/* arena / lifecycle (same shape as Piece 3's scene arena)                   */
/* ------------------------------------------------------------------------- */

#define LAYOUT_PAGE_SIZE 128
#define LAYOUT_MAX_PAGES 128

struct layout_arena {
    struct layout_node *pages[LAYOUT_MAX_PAGES];
    uint32_t            n_pages_allocated;
    uint32_t            free_list_head;
    uint32_t            next_never_used;
};

void layout_arena_init(struct layout_arena *a);
void layout_arena_destroy(struct layout_arena *a);

/* Create a leaf (is_container=false) as the last child of `parent`
 * (LAYOUT_HANDLE_NULL => root). Zero-init: SIZE_INTRINSIC on both axes,
 * grow/shrink 0. Caller fills in fields + scene_node. */
struct layout_handle layout_create_node(struct layout_arena *a, struct layout_handle parent);
void layout_destroy_node(struct layout_arena *a, struct layout_handle h);
struct layout_node *layout_resolve(struct layout_arena *a, struct layout_handle h);

/* Move `h` under `new_parent`, inserted AFTER `after` (LAYOUT_HANDLE_NULL =>
 * first child). Used by Piece 7's keyed reconciliation to keep sibling order
 * matching declared order after a reorder. */
void layout_reparent(struct layout_arena *a, struct layout_handle h,
                     struct layout_handle new_parent, struct layout_handle after);

/* ------------------------------------------------------------------------- */
/* the layout computation                                                    */
/* ------------------------------------------------------------------------- */

/* Run Phase 1 (intrinsic sizes, bottom-up) then Phase 2 (arrange, top-down)
 * for `root` given its assigned size (W,H) -- e.g. a window's surface size or
 * the compositor's screen size. Writes every node's resolved geometry into its
 * paired scene node. `sa` is the scene arena (for text metrics + write-back). */
void layout_run(struct layout_arena *la, struct scene_arena *sa,
                struct layout_handle root, float W, float H);

/* Word-wrap (with single-overlong-word character-wrap fallback) height of a
 * TEXT-paired node at a proposed width, in pixels. Exposed for tests + for
 * arrange's cross-axis text resolution. */
float layout_measure_height_at_width(struct layout_arena *la, struct scene_arena *sa,
                                     struct layout_handle text_node, float proposed_width);

/* Test/diagnostic: line count word-wrap produces at a proposed width. */
int layout_debug_wrap_lines(struct layout_arena *la, struct scene_arena *sa,
                            struct layout_handle text_node, float proposed_width);

#endif /* __EMBLINK_UI_LAYOUT_H__ */
