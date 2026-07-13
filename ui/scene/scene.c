/* ui/scene/scene.c -- EmbLink UI Piece 3 implementation (see scene.h).
 *
 * The load-bearing parts: a lazily-paged arena whose page addresses never
 * move (so intrusive HANDLE-based child lists stay valid across growth), and
 * a {index, generation} free-list whose generation is bumped on free -- the
 * exact ABA guard parent_pid uses for PCB slot reuse, reused here rather than
 * reinvented (invariant N2). */

#include "scene.h"

/* ---- pluggable page allocator (default malloc/free on a hosted build) ---- */

#ifndef SCENE_NO_DEFAULT_ALLOC
#include <stdlib.h>
static void *default_alloc(size_t n) { return malloc(n); }
static void  default_free(void *p)   { free(p); }
static void *(*g_alloc)(size_t) = default_alloc;
static void  (*g_free)(void *)  = default_free;
#else
/* Freestanding build (ring-3): caller MUST scene_set_allocator() before use. */
static void *(*g_alloc)(size_t) = 0;
static void  (*g_free)(void *)  = 0;
#endif

void scene_set_allocator(void *(*alloc_fn)(size_t), void (*free_fn)(void *)) {
    g_alloc = alloc_fn;
    g_free  = free_fn;
}

/* libc-free byte zero (avoids a memset dependency in freestanding builds). */
static void scene_zero(void *p, size_t n) {
    unsigned char *b = (unsigned char *)p;
    for (size_t i = 0; i < n; i++) b[i] = 0;
}

/* ---- arena internals ---------------------------------------------------- */

static struct scene_node *slot_ptr(struct scene_arena *a, uint32_t index) {
    uint32_t page = index / SCENE_PAGE_SIZE;
    uint32_t off  = index % SCENE_PAGE_SIZE;
    if (page >= SCENE_MAX_PAGES) return 0;
    if (!a->pages[page]) return 0;
    return &a->pages[page][off];
}

static bool ensure_page(struct scene_arena *a, uint32_t index) {
    uint32_t page = index / SCENE_PAGE_SIZE;
    if (page >= SCENE_MAX_PAGES) return false;
    if (a->pages[page]) return true;
    if (!g_alloc) return false;
    struct scene_node *p = (struct scene_node *)g_alloc(sizeof(struct scene_node) * SCENE_PAGE_SIZE);
    if (!p) return false;
    scene_zero(p, sizeof(struct scene_node) * SCENE_PAGE_SIZE);
    a->pages[page] = p;
    a->n_pages_allocated++;
    return true;
}

void scene_arena_init(struct scene_arena *a) {
    scene_zero(a, sizeof(*a));
    a->free_list_head  = 0;   /* 0 = empty free list (index 0 is reserved) */
    a->next_never_used = 1;   /* index 0 reserved as NODE_HANDLE_NULL */
}

void scene_arena_destroy(struct scene_arena *a) {
    for (uint32_t i = 0; i < SCENE_MAX_PAGES; i++) {
        if (a->pages[i] && g_free) g_free(a->pages[i]);
        a->pages[i] = 0;
    }
    a->n_pages_allocated = 0;
    a->free_list_head = 0;
    a->next_never_used = 1;
}

/* Pop a slot index (from the free list, else the high-water mark). 0 on OOM. */
static uint32_t alloc_slot(struct scene_arena *a) {
    if (a->free_list_head != 0) {
        uint32_t index = a->free_list_head;
        struct scene_node *n = slot_ptr(a, index);
        a->free_list_head = n ? n->next_sibling.index : 0;
        return index;
    }
    uint32_t index = a->next_never_used;
    if (index / SCENE_PAGE_SIZE >= SCENE_MAX_PAGES) return 0;   /* exhausted */
    if (!ensure_page(a, index)) return 0;
    a->next_never_used++;
    return index;
}

static void free_slot(struct scene_arena *a, uint32_t index) {
    struct scene_node *n = slot_ptr(a, index);
    if (!n) return;
    uint32_t gen = n->self.generation + 1;   /* bump on free -- the ABA guard */
    n->self.index = 0;                        /* self.index == 0 marks a FREE slot */
    n->self.generation = gen;                 /* preserved for the next reuse */
    n->next_sibling.index = a->free_list_head; /* thread onto the free list */
    n->next_sibling.generation = 0;
    a->free_list_head = index;
}

struct scene_node *scene_resolve(struct scene_arena *a, struct node_handle h) {
    if (h.index == 0) return 0;
    struct scene_node *n = slot_ptr(a, h.index);
    if (!n) return 0;
    if (n->self.index != h.index) return 0;          /* free slot or index mismatch */
    if (n->self.generation != h.generation) return 0; /* stale handle (N2) */
    return n;
}

/* Append child_h as the LAST child of parent_h (both assumed live). */
static void append_child(struct scene_arena *a, struct node_handle parent_h,
                         struct node_handle child_h) {
    struct scene_node *p = scene_resolve(a, parent_h);
    if (!p) return;
    if (node_handle_is_null(p->first_child)) {
        p->first_child = child_h;
        return;
    }
    struct node_handle c = p->first_child;
    for (;;) {
        struct scene_node *cn = scene_resolve(a, c);
        if (!cn || node_handle_is_null(cn->next_sibling)) {
            if (cn) cn->next_sibling = child_h;
            return;
        }
        c = cn->next_sibling;
    }
}

/* Splice child_h out of its parent's child list (no-op if it's a root). */
static void unlink_from_parent(struct scene_arena *a, struct node_handle child_h) {
    struct scene_node *c = scene_resolve(a, child_h);
    if (!c) return;
    struct node_handle parent_h = c->parent;
    struct scene_node *p = scene_resolve(a, parent_h);
    if (!p) { c->parent = NODE_HANDLE_NULL; return; }

    if (p->first_child.index == child_h.index) {
        p->first_child = c->next_sibling;
    } else {
        struct node_handle it = p->first_child;
        while (!node_handle_is_null(it)) {
            struct scene_node *in = scene_resolve(a, it);
            if (!in) break;
            if (in->next_sibling.index == child_h.index) {
                in->next_sibling = c->next_sibling;
                break;
            }
            it = in->next_sibling;
        }
    }
    c->next_sibling = NODE_HANDLE_NULL;
    c->parent = NODE_HANDLE_NULL;
}

struct node_handle scene_create_node(struct scene_arena *a, enum scene_node_kind kind,
                                     struct node_handle parent) {
    uint32_t index = alloc_slot(a);
    if (index == 0) return NODE_HANDLE_NULL;

    struct scene_node *n = slot_ptr(a, index);
    uint32_t gen = n->self.generation;   /* survived the last free (bumped there) */
    if (gen == 0) gen = 1;               /* never hand out generation 0 */
    scene_zero(n, sizeof(*n));

    n->self.index = index;
    n->self.generation = gen;
    n->kind = kind;
    n->parent = NODE_HANDLE_NULL;
    n->first_child = NODE_HANDLE_NULL;
    n->next_sibling = NODE_HANDLE_NULL;
    /* identity transform + opaque, so a node is sane before Layout runs (N1) */
    n->qw = 1.0f;
    n->sx = n->sy = n->sz = 1.0f;
    n->opacity = 1.0f;

    struct node_handle h = n->self;

    struct scene_node *p = scene_resolve(a, parent);
    if (p) {
        n->parent = parent;
        append_child(a, parent, h);
    }
    return h;
}

static void destroy_recursive(struct scene_arena *a, struct node_handle h) {
    struct scene_node *n = scene_resolve(a, h);
    if (!n) return;
    struct node_handle c = n->first_child;
    while (!node_handle_is_null(c)) {
        struct scene_node *cn = scene_resolve(a, c);
        struct node_handle next = cn ? cn->next_sibling : NODE_HANDLE_NULL;
        destroy_recursive(a, c);
        c = next;
    }
    free_slot(a, h.index);
}

void scene_destroy_node(struct scene_arena *a, struct node_handle h) {
    struct scene_node *n = scene_resolve(a, h);
    if (!n) return;
    unlink_from_parent(a, h);   /* keep the parent's child list consistent first */
    destroy_recursive(a, h);
}

void scene_reparent(struct scene_arena *a, struct node_handle h,
                    struct node_handle new_parent, struct node_handle after_sibling) {
    struct scene_node *n = scene_resolve(a, h);
    if (!n) return;
    struct scene_node *np = scene_resolve(a, new_parent);
    if (!np) return;   /* refuse to orphan onto an invalid parent */

    /* No-op reorder: already a child of new_parent in exactly this position.
     * An immediate-mode reconciler calls this every frame to keep sibling order
     * in sync; when order is unchanged it must NOT dirty the node (otherwise the
     * whole tree repaints every frame -- the dominant interactive-render cost). */
    if (n->parent.index == new_parent.index && n->parent.generation == new_parent.generation) {
        if (node_handle_is_null(after_sibling)) {
            if (np->first_child.index == h.index) return;              /* already first child */
        } else {
            struct scene_node *sib = scene_resolve(a, after_sibling);
            if (sib && sib->next_sibling.index == h.index) return;     /* already right after */
        }
    }

    unlink_from_parent(a, h);
    n->parent = new_parent;

    if (node_handle_is_null(after_sibling)) {
        /* insert as FIRST child */
        n->next_sibling = np->first_child;
        np->first_child = h;
    } else {
        struct scene_node *sib = scene_resolve(a, after_sibling);
        if (!sib || after_sibling.index == h.index) {  /* bad/self anchor -> append last */
            n->next_sibling = NODE_HANDLE_NULL;
            append_child(a, new_parent, h);
        } else {
            n->next_sibling = sib->next_sibling;
            sib->next_sibling = h;
            if (n->next_sibling.index == h.index)      /* never point at self */
                n->next_sibling = NODE_HANDLE_NULL;
        }
    }
    n->dirty = true;
}

/* ---- mutation setters --------------------------------------------------- */

/* Setters mark the node dirty ONLY when a value actually changes. This matters
 * for incremental rendering: an immediate-mode app re-runs layout every frame,
 * re-applying identical transforms/sizes to every node -- if that re-dirtied
 * the whole tree, the dirty-rect renderer would repaint everything every frame
 * (defeating its purpose). A no-op set must stay a no-op. */

static bool color_eq(struct color a, struct color b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}
static bool paint_eq(const struct paint *a, const struct paint *b) {
    if (a->kind != b->kind || a->n_stops != b->n_stops) return false;
    if (a->kind == PAINT_SOLID) return color_eq(a->solid, b->solid);
    return false;   /* gradients: conservatively treat as changed (rare) */
}

void scene_set_transform(struct scene_arena *a, struct node_handle h,
                         float tx, float ty, float tz,
                         float qx, float qy, float qz, float qw,
                         float sx, float sy, float sz) {
    struct scene_node *n = scene_resolve(a, h);
    if (!n) return;
    if (n->tx == tx && n->ty == ty && n->tz == tz &&
        n->qx == qx && n->qy == qy && n->qz == qz && n->qw == qw &&
        n->sx == sx && n->sy == sy && n->sz == sz) return;
    n->tx = tx; n->ty = ty; n->tz = tz;
    n->qx = qx; n->qy = qy; n->qz = qz; n->qw = qw;
    n->sx = sx; n->sy = sy; n->sz = sz;
    n->dirty = true;
}

void scene_set_size(struct scene_arena *a, struct node_handle h, float w, float ht) {
    struct scene_node *n = scene_resolve(a, h);
    if (!n) return;
    if (n->width == w && n->height == ht) return;
    n->width = w; n->height = ht; n->dirty = true;
}

void scene_set_paint(struct scene_arena *a, struct node_handle h, const struct paint *p) {
    struct scene_node *n = scene_resolve(a, h);
    if (!n || !p) return;
    if (paint_eq(&n->data.rect.fill, p)) return;
    n->data.rect.fill = *p;
    n->dirty = true;
}

void scene_set_text(struct scene_arena *a, struct node_handle h, const char *utf8,
                    uint32_t font_handle, float size_px, struct color color) {
    struct scene_node *n = scene_resolve(a, h);
    if (!n) return;
    /* Content-aware no-op guard: comparing only the POINTER hid in-place edits
     * (e.g. the home's clock snprintf's into the same buffer every second --
     * the text never re-rendered). Compare bytes; when equal, still adopt the
     * new pointer (the old one's lifetime belongs to the caller), no dirty. */
    int same_text = (n->data.text.utf8 == utf8);
    if (!same_text && n->data.text.utf8 && utf8) {
        const char *p = n->data.text.utf8, *q = utf8;
        while (*p && *p == *q) { p++; q++; }
        same_text = (*p == *q);
    }
    if (same_text && n->data.text.font_handle == font_handle &&
        n->data.text.size_px == size_px && color_eq(n->data.text.color, color)) {
        n->data.text.utf8 = utf8;
        return;
    }
    n->data.text.utf8 = utf8;
    n->data.text.font_handle = font_handle;
    n->data.text.size_px = size_px;
    n->data.text.color = color;
    n->dirty = true;
}

/* Explicit dirty for callers whose new content ALIASES the stored pointer (the
 * declare layer's per-instance shadow text buffer is overwritten in place
 * before the set call -- no setter-side comparison can see such an edit). */
void scene_mark_dirty(struct scene_arena *a, struct node_handle h) {
    struct scene_node *n = scene_resolve(a, h);
    if (n) n->dirty = true;
}

void scene_set_image(struct scene_arena *a, struct node_handle h, const void *pixels,
                     uint32_t w, uint32_t ht, enum embk_pixfmt fmt) {
    struct scene_node *n = scene_resolve(a, h);
    if (!n) return;
    if (n->data.image.pixels == pixels && n->data.image.w == w &&
        n->data.image.h == ht && n->data.image.fmt == fmt) return;
    n->data.image.pixels = pixels;
    n->data.image.w = w; n->data.image.h = ht; n->data.image.fmt = fmt;
    n->dirty = true;
}

void scene_set_shadow(struct scene_arena *a, struct node_handle h, bool enabled,
                      float dx, float dy, float blur, struct color color) {
    struct scene_node *n = scene_resolve(a, h);
    if (!n) return;
    if (n->shadow_enabled == enabled && n->shadow_dx == dx && n->shadow_dy == dy &&
        n->shadow_blur_radius == blur && color_eq(n->shadow_color, color)) return;
    n->shadow_enabled = enabled;
    n->shadow_dx = dx; n->shadow_dy = dy; n->shadow_blur_radius = blur;
    n->shadow_color = color;
    n->dirty = true;
}

void scene_set_border(struct scene_arena *a, struct node_handle h, float width, struct color color) {
    struct scene_node *n = scene_resolve(a, h);
    if (!n) return;
    if (n->border_width == width && color_eq(n->border_color, color)) return;
    n->border_width = width; n->border_color = color;
    n->dirty = true;
}

void scene_set_backdrop_blur(struct scene_arena *a, struct node_handle h, bool enabled, float radius) {
    struct scene_node *n = scene_resolve(a, h);
    if (!n) return;
    if (n->backdrop_blur_enabled == enabled && n->backdrop_blur_radius == radius) return;
    n->backdrop_blur_enabled = enabled;
    n->backdrop_blur_radius = radius;
    n->dirty = true;
}

void scene_set_opacity(struct scene_arena *a, struct node_handle h, float opacity) {
    struct scene_node *n = scene_resolve(a, h);
    if (!n) return;
    if (n->opacity == opacity) return;
    n->opacity = opacity; n->dirty = true;
}

void scene_set_clip_children(struct scene_arena *a, struct node_handle h, bool clip) {
    struct scene_node *n = scene_resolve(a, h);
    if (!n) return;
    if (n->clip_children == clip) return;
    n->clip_children = clip; n->dirty = true;
}

void scene_set_z(struct scene_arena *a, struct node_handle h, float z) {
    struct scene_node *n = scene_resolve(a, h);
    if (!n) return;
    if (n->z == z) return;
    n->z = z; n->dirty = true;
}

/* ---- math: TRS -> column-major mat4, and mat4 multiply ------------------ */

void scene_trs_to_matrix(const struct scene_node *n, float out[16]) {
    /* rotation matrix from the (assumed-normalized) quaternion */
    float x = n->qx, y = n->qy, z = n->qz, w = n->qw;
    float xx = x*x, yy = y*y, zz = z*z;
    float xy = x*y, xz = x*z, yz = y*z;
    float wx = w*x, wy = w*y, wz = w*z;

    float r00 = 1.0f - 2.0f*(yy + zz);
    float r01 = 2.0f*(xy - wz);
    float r02 = 2.0f*(xz + wy);
    float r10 = 2.0f*(xy + wz);
    float r11 = 1.0f - 2.0f*(xx + zz);
    float r12 = 2.0f*(yz - wx);
    float r20 = 2.0f*(xz - wy);
    float r21 = 2.0f*(yz + wx);
    float r22 = 1.0f - 2.0f*(xx + yy);

    /* column 0 = rotated x-axis * scale.x */
    out[0]  = r00 * n->sx; out[1]  = r10 * n->sx; out[2]  = r20 * n->sx; out[3]  = 0.0f;
    /* column 1 = rotated y-axis * scale.y */
    out[4]  = r01 * n->sy; out[5]  = r11 * n->sy; out[6]  = r21 * n->sy; out[7]  = 0.0f;
    /* column 2 = rotated z-axis * scale.z */
    out[8]  = r02 * n->sz; out[9]  = r12 * n->sz; out[10] = r22 * n->sz; out[11] = 0.0f;
    /* column 3 = translation */
    out[12] = n->tx; out[13] = n->ty; out[14] = n->tz; out[15] = 1.0f;
}

void scene_mat4_mul(const float a[16], const float b[16], float out[16]) {
    /* out = a * b, column-major: out[c*4+r] = sum_k a[k*4+r] * b[c*4+k] */
    float tmp[16];
    for (int c = 0; c < 4; c++) {
        for (int r = 0; r < 4; r++) {
            float s = 0.0f;
            for (int k = 0; k < 4; k++) s += a[k*4 + r] * b[c*4 + k];
            tmp[c*4 + r] = s;
        }
    }
    for (int i = 0; i < 16; i++) out[i] = tmp[i];
}

static void mat4_identity(float m[16]) {
    for (int i = 0; i < 16; i++) m[i] = 0.0f;
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

/* ---- traversal ---------------------------------------------------------- */

static void traverse_rec(struct scene_arena *a, struct node_handle h,
                         const struct scene_world *parent,
                         scene_visit_fn visit, void *ctx) {
    struct scene_node *n = scene_resolve(a, h);
    if (!n) return;

    struct scene_world w;
    float local[16];
    scene_trs_to_matrix(n, local);
    scene_mat4_mul(parent->world_matrix, local, w.world_matrix);
    w.world_z = parent->world_z + n->z;
    w.world_opacity = parent->world_opacity * n->opacity;

    if (visit) visit(h, n, &w, ctx);

    /* children in document order == PAINT order (back-to-front) */
    struct node_handle c = n->first_child;
    while (!node_handle_is_null(c)) {
        struct scene_node *cn = scene_resolve(a, c);
        struct node_handle next = cn ? cn->next_sibling : NODE_HANDLE_NULL;
        traverse_rec(a, c, &w, visit, ctx);
        c = next;
    }
}

void scene_traverse(struct scene_arena *a, struct node_handle root,
                    scene_visit_fn visit, void *ctx) {
    struct scene_world identity;
    mat4_identity(identity.world_matrix);
    identity.world_z = 0.0f;
    identity.world_opacity = 1.0f;
    traverse_rec(a, root, &identity, visit, ctx);
}

/* Recursive world-capture for a single target (used by compute_world). */
static bool compute_world_rec(struct scene_arena *a, struct node_handle h,
                              const struct scene_world *parent,
                              struct node_handle target, struct scene_world *out) {
    struct scene_node *n = scene_resolve(a, h);
    if (!n) return false;

    struct scene_world w;
    float local[16];
    scene_trs_to_matrix(n, local);
    scene_mat4_mul(parent->world_matrix, local, w.world_matrix);
    w.world_z = parent->world_z + n->z;
    w.world_opacity = parent->world_opacity * n->opacity;

    if (h.index == target.index && h.generation == target.generation) {
        *out = w;
        return true;
    }
    struct node_handle c = n->first_child;
    while (!node_handle_is_null(c)) {
        struct scene_node *cn = scene_resolve(a, c);
        struct node_handle next = cn ? cn->next_sibling : NODE_HANDLE_NULL;
        if (compute_world_rec(a, c, &w, target, out)) return true;
        c = next;
    }
    return false;
}

bool scene_compute_world(struct scene_arena *a, struct node_handle root,
                         struct node_handle target, struct scene_world *out) {
    struct scene_world identity;
    mat4_identity(identity.world_matrix);
    identity.world_z = 0.0f;
    identity.world_opacity = 1.0f;
    return compute_world_rec(a, root, &identity, target, out);
}

