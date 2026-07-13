/* ui/declare/declare_test.c -- EmbLink UI Piece 7 selftests (Section 8).
 * Pure userland.  make declare-test  -> exits 0 iff every T1..T7 holds. */

#include "ui.h"
#include "scene.h"
#include "layout.h"
#include "scope.h"
#include <stdio.h>

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  FAIL: %s\n", (msg)); g_fail++; } \
    else         { printf("  ok:   %s\n", (msg)); } \
} while (0)

static struct scene_arena  SA;
static struct layout_arena LA;

static int heq(struct instance_handle a, struct instance_handle b) {
    return a.index == b.index && a.generation == b.generation;
}
static void fresh(void) { scene_arena_init(&SA); layout_arena_init(&LA); ui_init(&SA, &LA); }
static void done(void)  { layout_arena_destroy(&LA); scene_arena_destroy(&SA); }

/* ---- T1: stable reuse + no-op skip ------------------------------------- */
static void app_two_texts(void) {
    ui_begin_vstack(0);
      ui_text("hello");
      ui_text("world");
    ui_end_stack();
}
static void t1_reuse(void) {
    printf("T1 stable reuse + no-op skip:\n");
    fresh();
    ui_frame_begin(); app_two_texts(); ui_frame_end();
    struct instance_handle vs = ui_first_child(ui_root());
    struct instance_handle t0 = ui_first_child(vs);
    struct instance_handle t1 = ui_next_sibling(t0);
    struct node_handle s0 = ui_scene_of(t0);
    uint32_t mut_after_first = ui_debug_mutation_count();

    ui_frame_begin(); app_two_texts(); ui_frame_end();     /* identical redeclare */
    struct instance_handle vs2 = ui_first_child(ui_root());
    struct instance_handle t0b = ui_first_child(vs2);
    struct instance_handle t1b = ui_next_sibling(t0b);

    CHECK(heq(vs, vs2) && heq(t0, t0b) && heq(t1, t1b), "same instance handles reused across identical passes");
    CHECK(ui_scene_of(t0b).index == s0.index, "same paired scene node reused");
    CHECK(ui_debug_mutation_count() == mut_after_first, "no mutations on an identical redeclare (no-op skip)");
    done();
}

/* ---- T2: sweep removes a dropped child --------------------------------- */
static int g_t2_count = 3;
static void app_n_children(void) {
    ui_begin_vstack(0);
      for (int i = 0; i < g_t2_count; i++) ui_text("item %d", i);
    ui_end_stack();
}
static void t2_sweep(void) {
    printf("T2 sweep on shrink:\n");
    fresh();
    g_t2_count = 3;
    ui_frame_begin(); app_n_children(); ui_frame_end();
    struct instance_handle vs = ui_first_child(ui_root());
    struct instance_handle c0 = ui_first_child(vs);
    struct instance_handle c1 = ui_next_sibling(c0);
    struct instance_handle c2 = ui_next_sibling(c1);
    struct node_handle   s2 = ui_scene_of(c2);
    struct layout_handle l2 = ui_layout_of(c2);
    CHECK(instance_resolve(c2) != 0, "3rd child exists after first pass");

    g_t2_count = 2;
    ui_frame_begin(); app_n_children(); ui_frame_end();     /* only 2 now */
    CHECK(instance_resolve(c2) == 0, "3rd instance destroyed by sweep");
    CHECK(scene_resolve(&SA, s2) == 0, "3rd child's scene node resolves NULL");
    CHECK(layout_resolve(&LA, l2) == 0, "3rd child's layout node resolves NULL");
    done();
}

/* ---- T3 (crux): fine-grained component re-running ---------------------- */
static struct signal_handle g_sigA, g_sigB;
static int g_runA, g_runB, g_runParent;
static void comp_A(void *p) { (void)p; int v=0; signal_get(g_sigA, &v, sizeof v); g_runA++; ui_text("A%d", v); }
static void comp_B(void *p) { (void)p; int v=0; signal_get(g_sigB, &v, sizeof v); g_runB++; ui_text("B%d", v); }
static void app_parent(void) {
    g_runParent++;
    ui_begin_vstack(0);
      int dummy = 0;
      ui_component(comp_A, &dummy, sizeof dummy, 1);
      ui_component(comp_B, &dummy, sizeof dummy, 2);
    ui_end_stack();
}
static void t3_fine_grained(void) {
    printf("T3 fine-grained component re-running:\n");
    fresh();
    int z = 0; g_sigA = signal_create(&z, sizeof z); g_sigB = signal_create(&z, sizeof z);
    g_runA = g_runB = g_runParent = 0;

    ui_frame_begin(); app_parent(); ui_frame_end();
    int a0 = g_runA, b0 = g_runB, p0 = g_runParent;
    CHECK(a0 == 1 && b0 == 1, "both components ran once on creation");

    int one = 1; signal_set(g_sigA, &one, sizeof one);   /* only A's signal changes */
    reactivity_flush();
    CHECK(g_runA == a0 + 1, "A re-ran exactly once (its signal changed)");
    CHECK(g_runB == b0,     "B did NOT re-run (its signal untouched)");
    CHECK(g_runParent == p0, "parent did NOT re-run (no ancestor cascade)");
    done();
}

/* ---- T4: props change forces a re-run ---------------------------------- */
static int g_runP;
static void comp_props(void *p) { (void)p; g_runP++; ui_text("x"); }
static int g_t4_prop = 10;
static void app_props(void) {
    ui_begin_vstack(0);
      ui_component(comp_props, &g_t4_prop, sizeof g_t4_prop, 5);
    ui_end_stack();
}
static void t4_props_change(void) {
    printf("T4 props-change re-run:\n");
    fresh();
    g_runP = 0;
    g_t4_prop = 10; ui_frame_begin(); app_props(); ui_frame_end();
    CHECK(g_runP == 1, "component ran on creation");

    /* redeclare with identical props -> no re-run */
    ui_frame_begin(); app_props(); ui_frame_end();
    CHECK(g_runP == 1, "identical props -> no re-run");

    /* redeclare with different props -> re-run (condition b) */
    g_t4_prop = 20; ui_frame_begin(); app_props(); ui_frame_end();
    CHECK(g_runP == 2, "changed props force a re-run even with no internal signal change");
    done();
}

/* ---- T5: keyed reorder preserves identity ------------------------------ */
static int g_t5_order[3] = {1,2,3};
static void app_keyed(void) {
    ui_begin_vstack(0);
      for (int i = 0; i < 3; i++) ui_text_keyed((uint64_t)g_t5_order[i], "k%d", g_t5_order[i]);
    ui_end_stack();
}
static void t5_keyed_reorder(void) {
    printf("T5 keyed reorder preserves identity:\n");
    fresh();
    g_t5_order[0]=1; g_t5_order[1]=2; g_t5_order[2]=3;
    ui_frame_begin(); app_keyed(); ui_frame_end();
    struct instance_handle vs = ui_first_child(ui_root());
    /* record handle of each key by scanning children */
    struct instance_handle by_key[4] = {0};
    for (struct instance_handle c = ui_first_child(vs); !instance_handle_is_null(c); c = ui_next_sibling(c)) {
        struct instance *ci = instance_resolve(c);
        if (ci) by_key[ci->explicit_key] = c;
    }
    struct node_handle sc1 = ui_scene_of(by_key[1]);

    g_t5_order[0]=3; g_t5_order[1]=1; g_t5_order[2]=2;      /* reorder */
    ui_frame_begin(); app_keyed(); ui_frame_end();
    struct instance_handle vs2 = ui_first_child(ui_root());
    struct instance_handle by_key2[4] = {0};
    for (struct instance_handle c = ui_first_child(vs2); !instance_handle_is_null(c); c = ui_next_sibling(c)) {
        struct instance *ci = instance_resolve(c);
        if (ci) by_key2[ci->explicit_key] = c;
    }
    CHECK(heq(by_key[1], by_key2[1]) && heq(by_key[2], by_key2[2]) && heq(by_key[3], by_key2[3]),
          "each key's instance handle identical before/after reorder");
    CHECK(ui_scene_of(by_key2[1]).index == sc1.index, "key 1's scene node preserved (state not reset)");
    /* and order actually changed: first child now key 3 */
    struct instance *first = instance_resolve(ui_first_child(vs2));
    CHECK(first && first->explicit_key == 3, "sibling order now reflects the reorder (key 3 first)");
    done();
}

/* ---- T6: hit-test clip-awareness --------------------------------------- */
static void t6_hit_clip(void) {
    printf("T6 hit-test clip-awareness:\n");
    fresh();
    /* clipping container 50x50 at origin; child 40x40 placed at x=30 (half
     * outside), clipped away past x=50. */
    ui_frame_begin();
    ui_box_begin(0);
      ui_set_size((struct layout_size){SIZE_FIXED,50,0,0,0}, (struct layout_size){SIZE_FIXED,50,0,0,0});
      ui_set_clip_children(true);
      ui_box_begin(0);
        ui_set_size((struct layout_size){SIZE_FIXED,40,0,0,0}, (struct layout_size){SIZE_FIXED,40,0,0,0});
      ui_box_end();
    ui_box_end();
    ui_frame_end();

    /* place the child manually at x=30 (layout would stack it at 0; we want it
     * straddling the clip edge for the test) */
    struct instance_handle outer = ui_first_child(ui_root());
    struct instance_handle inner = ui_first_child(outer);
    ui_run_layout(200, 200);
    /* override inner's scene transform to x=30 so its right half is clipped */
    scene_set_transform(&SA, ui_scene_of(inner), 30, 5, 0, 0,0,0,1, 1,1,1);

    /* a click at x=40 (inside inner's rect 30..70, but inside clip 0..50) hits */
    ui_dispatch_click(40, 20);
    CHECK(ui_consume_click(inner), "click inside the visible (unclipped) part hits");

    /* a click at x=60 (inside inner's rect 30..70 but OUTSIDE clip 0..50) misses */
    ui_dispatch_click(60, 20);
    CHECK(!ui_consume_click(inner), "click in the clipped-away part does NOT hit");
    done();
}

/* ---- T7: button click is a one-frame pulse ----------------------------- */
static bool g_last_click;
static void app_button(void) {
    ui_begin_vstack(0);
      g_last_click = ui_button("go");
    ui_end_stack();
}
static void t7_button_pulse(void) {
    printf("T7 button click one-frame pulse:\n");
    fresh();
    ui_frame_begin(); app_button(); ui_frame_end();
    ui_run_layout(200, 200);

    /* find the button box (first child of the vstack) and click its center */
    struct instance_handle vs = ui_first_child(ui_root());
    struct instance_handle btn = ui_first_child(vs);
    struct scene_node *bs = scene_resolve(&SA, ui_scene_of(btn));
    /* place + size the button so we can click it deterministically */
    scene_set_transform(&SA, ui_scene_of(btn), 10, 10, 0, 0,0,0,1, 1,1,1);
    if (bs) { bs->width = 40; bs->height = 20; }

    ui_dispatch_click(20, 15);
    ui_frame_begin(); app_button(); ui_frame_end();       /* pass that reads the flag */
    CHECK(g_last_click, "button returns true on the declare pass after the click");

    ui_frame_begin(); app_button(); ui_frame_end();       /* next pass */
    CHECK(!g_last_click, "button returns false on the following pass (pulse, not sticky)");
    done();
}

int main(void) {
    printf("=== EmbLink UI Piece 7: declarative-API selftests ===\n");
    t1_reuse();
    t2_sweep();
    t3_fine_grained();
    t4_props_change();
    t5_keyed_reorder();
    t6_hit_clip();
    t7_button_pulse();
    printf("=== declare-test: %s (%d failures) ===\n", g_fail ? "FAIL" : "OK", g_fail);
    return g_fail ? 1 : 0;
}
