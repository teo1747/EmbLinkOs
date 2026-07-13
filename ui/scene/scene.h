#ifndef __EMBLINK_UI_SCENE_H__
#define __EMBLINK_UI_SCENE_H__

/* ui/scene/scene.h -- EmbLink UI Piece 3: the SCENE TREE.
 *
 * A RESOLVED RENDER IR (intermediate representation) -- NOT what an app
 * author writes. It has no flex/stack/padding concept: Layout (Piece 5)
 * resolves an app's declarative tree (Piece 7) down INTO this one, so every
 * node here already carries a concrete size and transform. This is the same
 * kind of thing as a Skia display list, and that narrowness is what keeps it
 * small and stable.
 *
 * Pure userland C: no kernel primitives, no syscalls. Used by BOTH a client
 * (building its own window's content) and the compositor (compositing all
 * windows/panels/background with 3D transforms) -- one library, two consumers.
 *
 * Freestanding-clean: depends only on <stdint.h>/<stdbool.h>/<stddef.h> and,
 * for page allocation ONLY, a caller-pluggable allocator (defaults to
 * malloc/free on a hosted build). No libc math -- TRS composition is done
 * with plain float arithmetic here. */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Opaque pixel-format tag for IMAGE nodes -- Piece 4 (backend) interprets it;
 * Piece 3 only stores it. Mirrors user/lib/embk.h's EMBK_PIXFMT_* value so the
 * two never disagree, but declared here so the scene lib stays SDK-independent
 * (it must host-compile without the syscall SDK). */
#ifndef EMBK_PIXFMT_BGRA8888_PRE
enum embk_pixfmt { EMBK_PIXFMT_NONE = 0, EMBK_PIXFMT_BGRA8888_PRE = 1 };
#else
enum embk_pixfmt { EMBK_PIXFMT_SCENE_NONE = 0 };
#endif

/* ------------------------------------------------------------------------- */
/* Section 1: node handles & the growable paged arena                        */
/* ------------------------------------------------------------------------- */

struct node_handle {
    uint32_t index;       /* flat node index; 0 is reserved (never a real node) */
    uint32_t generation;  /* ABA guard -- bumped on free, so a stale handle to a
                           * reused slot resolves to NULL (invariant N2) */
};

/* index 0 reserved, never a real node -> a zeroed handle is "null". */
static const struct node_handle NODE_HANDLE_NULL = { 0, 0 };

static inline bool node_handle_is_null(struct node_handle h) {
    return h.index == 0;
}

#define SCENE_PAGE_SIZE 256   /* nodes per page */
#define SCENE_MAX_PAGES 256   /* -> up to 65536 nodes; pages are lazy, so unused
                               * capacity costs one NULL pointer each */

struct scene_node; /* defined below */

struct scene_arena {
    struct scene_node *pages[SCENE_MAX_PAGES]; /* NULL until a page is allocated */
    uint32_t           n_pages_allocated;
    uint32_t           free_list_head;  /* flat index of first free slot, 0 = none */
    uint32_t           next_never_used; /* high-water mark; starts at 1 (0 reserved) */
};

/* ------------------------------------------------------------------------- */
/* Section 2: the node                                                       */
/* ------------------------------------------------------------------------- */

enum scene_node_kind {
    SCENE_NODE_GROUP = 0,  /* no visual of its own; pure transform+clip container */
    SCENE_NODE_RECT,       /* solid/gradient rounded rect */
    SCENE_NODE_IMAGE,      /* a bitmap fill */
    SCENE_NODE_TEXT,       /* a shaped text run */
};

struct color { float r, g, b, a; };   /* STRAIGHT (non-premultiplied) alpha,
                                        * author-facing -- distinct from Piece
                                        * 1's premultiplied BGRA8888 pixels; the
                                        * backend converts at raster time. */

enum paint_kind { PAINT_NONE = 0, PAINT_SOLID, PAINT_LINEAR_GRADIENT, PAINT_RADIAL_GRADIENT };
struct gradient_stop { float offset; struct color color; };  /* offset 0..1 */

struct paint {
    enum paint_kind kind;
    struct color    solid;                        /* PAINT_SOLID */
    struct gradient_stop stops[8];
    uint8_t         n_stops;                       /* PAINT_*_GRADIENT */
    float           angle_deg;                     /* PAINT_LINEAR_GRADIENT direction */
    float           center_x, center_y, radius;    /* PAINT_RADIAL_GRADIENT, node-local */
};

struct scene_node {
    struct node_handle self;                       /* for validation/debug; self.index
                                                    * == 0 marks a FREE slot */
    struct node_handle parent;
    struct node_handle first_child, next_sibling;  /* intrusive child list via HANDLES
                                                    * (survives arena page growth);
                                                    * next_sibling doubles as the
                                                    * free-list link when freed */
    enum scene_node_kind kind;

    /* --- local transform (TRS, decomposed) --- */
    float tx, ty, tz;
    float qx, qy, qz, qw;    /* rotation quaternion; identity = (0,0,0,1) */
    float sx, sy, sz;         /* scale; identity = (1,1,1) */

    /* --- geometry, resolved by Layout (Piece 5); Piece 3 never computes these */
    float width, height;
    float corner_radius;      /* RECT only; 0 = sharp */

    /* --- stacking --- */
    float z;                   /* TRUE depth, depth-aware backends only. Paint order
                                * is document order, NOT z (Section 0). */

    /* --- clipping --- */
    bool  clip_children;

    /* --- border (RECT; a hairline stroke drawn INSIDE the rounded edge, on
     * top of the fill) --- */
    float border_width;        /* 0 = no border */
    struct color border_color;

    /* --- effects --- */
    bool  shadow_enabled;
    float shadow_dx, shadow_dy, shadow_blur_radius;
    struct color shadow_color;

    bool  backdrop_blur_enabled;
    float backdrop_blur_radius;

    float opacity;              /* 0..1; multiplies the WHOLE subtree in traversal */

    /* --- kind-specific payload --- */
    union {
        struct { struct paint fill; } rect;
        struct { const void *pixels; uint32_t w, h; enum embk_pixfmt fmt; } image;
        struct { const char *utf8; uint32_t font_handle; float size_px; struct color color; } text;
    } data;

    bool  dirty;   /* any mutation sets this; lets a caching traversal skip an
                    * unchanged subtree's world recompute */

    /* Opaque tag Piece 3 never interprets -- exists purely so a consumer above
     * (Piece 7's declarative layer) can stash its own identifier (e.g. a packed
     * instance_handle) on the paired scene node, so hit-testing can map a
     * struct node_handle back to whatever owns it. Zero-initialized. */
    uint64_t user_data;
};

/* ------------------------------------------------------------------------- */
/* Section 1 (API): lifecycle                                                */
/* ------------------------------------------------------------------------- */

/* Optional: install a custom page allocator (both must be set, or neither).
 * Defaults to malloc/free. `alloc(size)` must return zeroed memory or NULL. */
void scene_set_allocator(void *(*alloc_fn)(size_t), void (*free_fn)(void *));

void scene_arena_init(struct scene_arena *a);
void scene_arena_destroy(struct scene_arena *a);   /* frees every allocated page */

/* Create a node of `kind` as the LAST child of `parent` (NODE_HANDLE_NULL =>
 * a root, no parent). Zero-initialized (identity transform, zero size,
 * opacity 1). Returns NODE_HANDLE_NULL if the arena is exhausted. */
struct node_handle scene_create_node(struct scene_arena *a, enum scene_node_kind kind,
                                     struct node_handle parent);

/* Destroy `h` AND all its descendants; each slot is freed with a bumped
 * generation (N2). No-op on a stale/invalid handle. */
void scene_destroy_node(struct scene_arena *a, struct node_handle h);

/* Resolve to a live node pointer, or NULL if the handle is stale/invalid
 * (index out of range, page not allocated, slot free, or generation mismatch).
 * NEVER hold the returned pointer across a call that may allocate -- re-resolve. */
struct scene_node *scene_resolve(struct scene_arena *a, struct node_handle h);

/* ------------------------------------------------------------------------- */
/* Section 3: mutation API (Piece 7's reconciler / Piece 5 layout call these) */
/* Each resolves the handle (stale => silent no-op), writes, sets dirty=true. */
/* ------------------------------------------------------------------------- */

void scene_set_transform(struct scene_arena *a, struct node_handle h,
                         float tx, float ty, float tz,
                         float qx, float qy, float qz, float qw,
                         float sx, float sy, float sz);
void scene_set_size(struct scene_arena *a, struct node_handle h, float w, float ht);
void scene_set_paint(struct scene_arena *a, struct node_handle h, const struct paint *p); /* RECT */
void scene_set_text(struct scene_arena *a, struct node_handle h, const char *utf8,
                    uint32_t font_handle, float size_px, struct color color);

/* Force-dirty a node whose (aliased) content was edited in place. */
void scene_mark_dirty(struct scene_arena *a, struct node_handle h);
void scene_set_image(struct scene_arena *a, struct node_handle h, const void *pixels,
                     uint32_t w, uint32_t ht, enum embk_pixfmt fmt);
void scene_set_shadow(struct scene_arena *a, struct node_handle h, bool enabled,
                      float dx, float dy, float blur, struct color color);
void scene_set_border(struct scene_arena *a, struct node_handle h, float width, struct color color);
void scene_set_backdrop_blur(struct scene_arena *a, struct node_handle h, bool enabled, float radius);
void scene_set_opacity(struct scene_arena *a, struct node_handle h, float opacity);
void scene_set_clip_children(struct scene_arena *a, struct node_handle h, bool clip);
void scene_set_z(struct scene_arena *a, struct node_handle h, float z);

/* Move `h` to be a child of `new_parent`, inserted AFTER `after_sibling`
 * (NODE_HANDLE_NULL => first child). Local transform is preserved; world
 * transform is recomputed by the next traversal. */
void scene_reparent(struct scene_arena *a, struct node_handle h,
                    struct node_handle new_parent, struct node_handle after_sibling);

/* ------------------------------------------------------------------------- */
/* Section 4: traversal & world-transform composition                        */
/* ------------------------------------------------------------------------- */

struct scene_world {           /* computed fresh each traversal; NOT stored on the node */
    float world_matrix[16];     /* parent_world * local_TRS, column-major */
    float world_z;              /* parent_world_z + local z */
    float world_opacity;        /* parent_world_opacity * local opacity */
};

typedef void (*scene_visit_fn)(struct node_handle h, const struct scene_node *node,
                               const struct scene_world *world, void *ctx);

/* Depth-first from `root`; children in first_child/next_sibling order == PAINT
 * ORDER (back-to-front, painter's algorithm). `visit` is called per node with
 * its freshly-composed world state, then children recurse with that state as
 * their parent. */
void scene_traverse(struct scene_arena *a, struct node_handle root,
                    scene_visit_fn visit, void *ctx);

/* Exposed for callers/tests that want a node's world transform without a full
 * visit callback (e.g. hit-testing later). Returns false if `h` is unreachable
 * from `root`. */
bool scene_compute_world(struct scene_arena *a, struct node_handle root,
                         struct node_handle target, struct scene_world *out);

/* --- small math helpers, exposed for tests --- */
void scene_trs_to_matrix(const struct scene_node *n, float out[16]);
void scene_mat4_mul(const float a[16], const float b[16], float out[16]); /* out = a*b */


#endif /* __EMBLINK_UI_SCENE_H__ */
