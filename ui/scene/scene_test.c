/* ui/scene/scene_test.c -- EmbLink UI Piece 3 selftests (Section 7).
 *
 * PURE USERLAND UNIT TESTS -- no QEMU / no ring-3 needed for this piece (the
 * scene tree has no syscalls). Host-compiled and run natively:
 *     make scene-test
 * Exits 0 iff every S1..S5 invariant holds. */

#include "scene.h"
#include <stdio.h>
#include <stdlib.h>

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  FAIL: %s\n", (msg)); g_fail++; } \
} while (0)

static int feq(float a, float b) {
    float d = a - b; if (d < 0) d = -d;
    return d < 1e-4f;
}

/* ---- S1: transform composition ----------------------------------------- */
static void s1_transform_composition(void) {
    printf("S1 transform composition:\n");
    struct scene_arena a; scene_arena_init(&a);

    struct node_handle group = scene_create_node(&a, SCENE_NODE_GROUP, NODE_HANDLE_NULL);
    struct node_handle rect  = scene_create_node(&a, SCENE_NODE_RECT, group);
    /* identity quaternion (0,0,0,1), unit scale */
    scene_set_transform(&a, group, 10,10,0,  0,0,0,1,  1,1,1);
    scene_set_transform(&a, rect,   5, 5,0,  0,0,0,1,  1,1,1);

    struct scene_world w;
    CHECK(scene_compute_world(&a, group, rect, &w), "rect reachable");
    CHECK(feq(w.world_matrix[12], 15) && feq(w.world_matrix[13], 15) && feq(w.world_matrix[14], 0),
          "child world translate == (15,15,0)");

    /* move the group; child's world must follow WITHOUT touching the child */
    scene_set_transform(&a, group, 20,0,0,  0,0,0,1,  1,1,1);
    CHECK(scene_compute_world(&a, group, rect, &w), "rect still reachable");
    CHECK(feq(w.world_matrix[12], 25) && feq(w.world_matrix[13], 5) && feq(w.world_matrix[14], 0),
          "child world translate recomposed to (25,5,0)");

    scene_arena_destroy(&a);
}

/* ---- S2: paint order is document order, independent of z ---------------- */
static struct node_handle g_order[8];
static int g_order_n;
static void record_visit(struct node_handle h, const struct scene_node *n,
                         const struct scene_world *w, void *ctx) {
    (void)n; (void)w; (void)ctx;
    if (g_order_n < 8) g_order[g_order_n++] = h;
}
static void s2_paint_order(void) {
    printf("S2 paint order == document order:\n");
    struct scene_arena a; scene_arena_init(&a);

    struct node_handle root = scene_create_node(&a, SCENE_NODE_GROUP, NODE_HANDLE_NULL);
    struct node_handle A = scene_create_node(&a, SCENE_NODE_RECT, root);
    struct node_handle B = scene_create_node(&a, SCENE_NODE_RECT, root);
    struct node_handle C = scene_create_node(&a, SCENE_NODE_RECT, root);
    scene_set_z(&a, A, 5); scene_set_z(&a, B, 1); scene_set_z(&a, C, 3);

    g_order_n = 0;
    scene_traverse(&a, root, record_visit, 0);
    /* order: root, A, B, C -- children strictly in document order, NOT z */
    CHECK(g_order_n == 4, "visited root + 3 children");
    CHECK(g_order[1].index == A.index, "first child visited is A");
    CHECK(g_order[2].index == B.index, "second child visited is B");
    CHECK(g_order[3].index == C.index, "third child visited is C");

    scene_arena_destroy(&a);
}

/* ---- S3: the ABA guard (N2) -- the important one ------------------------ */
static void s3_aba_guard(void) {
    printf("S3 ABA generation guard:\n");
    struct scene_arena a; scene_arena_init(&a);

    struct node_handle h1 = scene_create_node(&a, SCENE_NODE_RECT, NODE_HANDLE_NULL);
    CHECK(scene_resolve(&a, h1) != NULL, "h1 initially resolves");
    uint32_t reused_index = h1.index;

    scene_destroy_node(&a, h1);
    CHECK(scene_resolve(&a, h1) == NULL, "resolve after destroy is NULL");

    struct node_handle h2 = scene_create_node(&a, SCENE_NODE_RECT, NODE_HANDLE_NULL);
    CHECK(h2.index == reused_index, "new node reused the freed slot index");
    CHECK(h2.generation != h1.generation, "generation was bumped on reuse");
    CHECK(scene_resolve(&a, h2) != NULL, "new handle h2 resolves");
    /* the crux: the STALE handle must NOT alias onto the new occupant */
    CHECK(scene_resolve(&a, h1) == NULL, "stale h1 resolves NULL even though slot is live");

    scene_arena_destroy(&a);
}

/* ---- S4: arena growth is real ------------------------------------------- */
static void s4_arena_growth(void) {
    printf("S4 arena growth across a page boundary:\n");
    struct scene_arena a; scene_arena_init(&a);

    int total = SCENE_PAGE_SIZE + 10;
    struct node_handle *hs = (struct node_handle *)malloc(sizeof(struct node_handle) * total);
    for (int i = 0; i < total; i++)
        hs[i] = scene_create_node(&a, SCENE_NODE_RECT, NODE_HANDLE_NULL);

    CHECK(a.n_pages_allocated == 2, "a second page was actually allocated");

    int all_ok = 1, saw_p0 = 0, saw_p1 = 0;
    for (int i = 0; i < total; i++) {
        if (scene_resolve(&a, hs[i]) == NULL) all_ok = 0;
        if (hs[i].index < SCENE_PAGE_SIZE) saw_p0 = 1; else saw_p1 = 1;
    }
    CHECK(all_ok, "every handle across both pages resolves");
    CHECK(saw_p0 && saw_p1, "handles landed in both page 0 and page 1");

    free(hs);
    scene_arena_destroy(&a);
}

/* ---- S5: reparent preserves world-correctness --------------------------- */
static void s5_reparent(void) {
    printf("S5 reparent recomputes world from live structure:\n");
    struct scene_arena a; scene_arena_init(&a);

    struct node_handle A = scene_create_node(&a, SCENE_NODE_GROUP, NODE_HANDLE_NULL);
    struct node_handle B = scene_create_node(&a, SCENE_NODE_GROUP, A);
    struct node_handle C = scene_create_node(&a, SCENE_NODE_RECT, B);
    scene_set_transform(&a, A, 100,0,0, 0,0,0,1, 1,1,1);
    scene_set_transform(&a, B,  10,20,0, 0,0,0,1, 1,1,1);
    scene_set_transform(&a, C,   1, 2,0, 0,0,0,1, 1,1,1);

    struct scene_world w;
    CHECK(scene_compute_world(&a, A, C, &w), "C reachable under A->B->C");
    CHECK(feq(w.world_matrix[12], 111) && feq(w.world_matrix[13], 22),
          "C world translate == (111,22,0) via A*B*C");

    /* move C to be A's direct child; local transform unchanged */
    scene_reparent(&a, C, A, NODE_HANDLE_NULL);
    CHECK(scene_compute_world(&a, A, C, &w), "C still reachable after reparent");
    CHECK(feq(w.world_matrix[12], 101) && feq(w.world_matrix[13], 2),
          "C world recomputed to A*C == (101,2,0) -- B's contribution gone");

    scene_arena_destroy(&a);
}

int main(void) {
    printf("=== EmbLink UI Piece 3: scene-tree selftests ===\n");
    s1_transform_composition();
    s2_paint_order();
    s3_aba_guard();
    s4_arena_growth();
    s5_reparent();
    printf("=== scene-test: %s (%d failures) ===\n", g_fail ? "FAIL" : "OK", g_fail);
    return g_fail ? 1 : 0;
}
