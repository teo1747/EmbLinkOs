/* ui/reactive/reactive_test.c -- EmbLink UI Piece 6 selftests (Section 8).
 * Pure userland.  make reactive-test  -> exits 0 iff every T1..T6 holds. */

#include "scope.h"
#include "signal.h"
#include <stdio.h>

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  FAIL: %s\n", (msg)); g_fail++; } \
    else         { printf("  ok:   %s\n", (msg)); } \
} while (0)

/* a scope that reads one signal and counts its runs */
struct one_ctx { struct signal_handle s; int runs; };
static void read_one(void *ctx) {
    struct one_ctx *c = ctx; int v = 0; signal_get(c->s, &v, sizeof v); c->runs++;
}

/* a scope that reads A or B depending on a mode flag (T2) */
struct branch_ctx { struct signal_handle a, b; int mode; };
static void read_branch(void *ctx) {
    struct branch_ctx *c = ctx; int v = 0;
    if (c->mode == 0) signal_get(c->a, &v, sizeof v);
    else              signal_get(c->b, &v, sizeof v);
}

/* ---- T1: fine-grained exactness ---------------------------------------- */
static void t1_fine_grained(void) {
    printf("T1 fine-grained dependency exactness:\n");
    reactivity_test_reset();
    int za = 1, zb = 2;
    struct signal_handle A = signal_create(&za, sizeof za);
    struct signal_handle B = signal_create(&zb, sizeof zb);
    struct one_ctx c1 = { A, 0 }, c2 = { B, 0 };
    struct scope_handle s1 = scope_create(read_one, &c1);
    struct scope_handle s2 = scope_create(read_one, &c2);
    scope_rerun(s1); scope_rerun(s2);        /* establish edges A->s1, B->s2 */

    int na = 42; signal_set(A, &na, sizeof na);
    CHECK(scope_is_dirty(s1),  "scope reading A is dirty after set(A)");
    CHECK(!scope_is_dirty(s2), "scope reading only B is NOT dirty after set(A)");
}

/* ---- T2 (the crux): stale-edge clearing -------------------------------- */
static void t2_stale_edges(void) {
    printf("T2 stale-edge clearing on re-run:\n");
    reactivity_test_reset();
    int z = 0;
    struct signal_handle A = signal_create(&z, sizeof z);
    struct signal_handle B = signal_create(&z, sizeof z);
    struct branch_ctx c = { A, B, 0 };       /* mode 0 -> reads A */
    struct scope_handle s = scope_create(read_branch, &c);
    scope_rerun(s);                          /* run 1: depends on A only */

    c.mode = 1;                              /* now the body reads B instead */
    scope_rerun(s);                          /* run 2: Step 1 must clear the A edge */

    int v1 = 1; signal_set(A, &v1, sizeof v1);
    CHECK(!scope_is_dirty(s), "set(A) does NOT dirty the scope (stale A edge was cleared)");
    int v2 = 2; signal_set(B, &v2, sizeof v2);
    CHECK(scope_is_dirty(s),  "set(B) DOES dirty the scope (fresh B edge)");
}

/* ---- T3: no-op write skip ---------------------------------------------- */
static void t3_noop(void) {
    printf("T3 no-op write skip:\n");
    reactivity_test_reset();
    int val = 7;
    struct signal_handle S = signal_create(&val, sizeof val);
    struct one_ctx c = { S, 0 };
    struct scope_handle sc = scope_create(read_one, &c);
    scope_rerun(sc);
    uint32_t gen_before = signal_generation(S);

    int same = 7; signal_set(S, &same, sizeof same);   /* identical bytes */
    CHECK(!scope_is_dirty(sc), "no dependent marked dirty on a no-op write");
    CHECK(signal_generation(S) == gen_before, "generation unchanged on a no-op write");

    int diff = 8; signal_set(S, &diff, sizeof diff);
    CHECK(scope_is_dirty(sc), "a real (changed) write DOES dirty + bump generation");
    CHECK(signal_generation(S) == gen_before + 1, "generation bumped exactly once on real write");
}

/* ---- T4: nesting attribution ------------------------------------------- */
static struct signal_handle g_S, g_T1, g_T2;
static struct scope_handle g_B;
static void body_B(void *ctx) { (void)ctx; int v = 0; signal_get(g_S, &v, sizeof v); }
static void body_A(void *ctx) {
    (void)ctx; int v = 0;
    signal_get(g_T1, &v, sizeof v);   /* attributes to A */
    scope_rerun(g_B);                 /* nested run: B reads S */
    signal_get(g_T2, &v, sizeof v);   /* attributes back to A after B returns */
}
static void t4_nesting(void) {
    printf("T4 nesting attribution (stack restore):\n");
    reactivity_test_reset();
    int z = 0;
    g_S = signal_create(&z, sizeof z); g_T1 = signal_create(&z, sizeof z); g_T2 = signal_create(&z, sizeof z);
    g_B = scope_create(body_B, 0);
    struct scope_handle A = scope_create(body_A, 0);
    scope_rerun(A);

    CHECK(signal_has_dependent(g_S, g_B),  "S attributes to nested scope B");
    CHECK(!signal_has_dependent(g_S, A),   "S does NOT attribute to outer scope A");
    CHECK(signal_has_dependent(g_T1, A),   "T1 (read before nesting) attributes to A");
    CHECK(signal_has_dependent(g_T2, A),   "T2 (read after nesting returns) attributes back to A");
}

/* ---- T5: destroyed-while-dirty safety ---------------------------------- */
static void t5_destroyed_while_dirty(void) {
    printf("T5 destroyed-while-dirty safety:\n");
    reactivity_test_reset();
    int z = 0;
    struct signal_handle S = signal_create(&z, sizeof z);
    struct one_ctx c = { S, 0 };
    struct scope_handle B = scope_create(read_one, &c);
    scope_rerun(B);                          /* runs once; edge S->B */
    int runs_after_setup = c.runs;

    int v = 5; signal_set(S, &v, sizeof v);  /* enqueues B's handle as dirty */
    CHECK(reactivity_dirty_count() >= 1, "B enqueued dirty");
    scope_destroy(B);                        /* slot freed/reused before flush */

    reactivity_flush();                      /* must skip the stale handle safely */
    CHECK(c.runs == runs_after_setup, "destroyed scope was NOT re-run by flush");
    CHECK(reactivity_dirty_count() == 0, "worklist drained cleanly");
}

/* ---- T6: batching / dedup ---------------------------------------------- */
struct two_ctx { struct signal_handle a, b; int runs; };
static void read_two(void *ctx) {
    struct two_ctx *c = ctx; int v = 0;
    signal_get(c->a, &v, sizeof v);
    signal_get(c->b, &v, sizeof v);
    c->runs++;
}
static void t6_batching(void) {
    printf("T6 batching + dedup:\n");
    reactivity_test_reset();
    int z = 0;
    struct signal_handle A = signal_create(&z, sizeof z);
    struct signal_handle B = signal_create(&z, sizeof z);
    struct two_ctx c = { A, B, 0 };
    struct scope_handle s = scope_create(read_two, &c);
    scope_rerun(s);                          /* runs once; edges A->s, B->s */
    int base = c.runs;

    int va = 1; signal_set(A, &va, sizeof va);
    int vb = 1; signal_set(B, &vb, sizeof vb);
    CHECK(reactivity_dirty_count() == 1, "scope enqueued exactly ONCE despite two writes");

    reactivity_flush();
    CHECK(c.runs == base + 1, "scope body re-ran exactly once, not twice");
}

int main(void) {
    printf("=== EmbLink UI Piece 6: reactivity selftests ===\n");
    t1_fine_grained();
    t2_stale_edges();
    t3_noop();
    t4_nesting();
    t5_destroyed_while_dirty();
    t6_batching();
    printf("=== reactive-test: %s (%d failures) ===\n", g_fail ? "FAIL" : "OK", g_fail);
    return g_fail ? 1 : 0;
}
