/* ui/reactive/reactive.c -- EmbLink UI Piece 6 implementation
 * (see signal.h / scope.h).
 *
 * Signals, scopes, and edges live in generation-guarded pools (the same ABA
 * discipline as Pieces 3/5 -- a stale handle to a reused slot resolves NULL).
 * Edges are pointer-referenced (the intrusive doubly-linked lists), so their
 * pool hands out stable addresses. Fixed-capacity here for simplicity; swapping
 * to the paged lazy arenas of scene/layout is mechanical if a cap is ever hit. */

#include "scope.h"
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* pools                                                                     */
/* ------------------------------------------------------------------------- */

#define SIGNAL_MAX          4096
#define SCOPE_MAX           4096
#define EDGE_MAX            16384
#define SCOPE_STACK_MAX     64
#define DIRTY_WORKLIST_MAX  4096

static struct signal g_sig[SIGNAL_MAX];
static uint32_t g_sig_hw = 1;                 /* index 0 reserved */
static uint32_t g_sig_free[SIGNAL_MAX]; static int g_sig_free_top = -1;

static struct scope g_scope[SCOPE_MAX];
static uint32_t g_scope_hw = 1;
static uint32_t g_scope_free[SCOPE_MAX]; static int g_scope_free_top = -1;

static struct scope_edge g_edge[EDGE_MAX];
static uint32_t g_edge_hw = 0;
static struct scope_edge *g_edge_free_head;

static struct scope *g_scope_stack[SCOPE_STACK_MAX];
static int g_scope_stack_top = -1;

static struct scope_handle g_dirty_worklist[DIRTY_WORKLIST_MAX];
static uint32_t g_dirty_count;

/* ---- edge alloc/free (pointer-stable pool + free list via next_in_scope) - */
static struct scope_edge *edge_arena_alloc(void) {
    struct scope_edge *e;
    if (g_edge_free_head) { e = g_edge_free_head; g_edge_free_head = e->next_in_scope; }
    else if (g_edge_hw < EDGE_MAX) e = &g_edge[g_edge_hw++];
    else return 0;
    memset(e, 0, sizeof(*e));
    return e;
}
static void edge_arena_free(struct scope_edge *e) {
    e->next_in_scope = g_edge_free_head; g_edge_free_head = e;
}

/* ------------------------------------------------------------------------- */
/* signals                                                                   */
/* ------------------------------------------------------------------------- */

struct signal *signal_resolve(struct signal_handle h) {
    if (h.index == 0 || h.index >= SIGNAL_MAX) return 0;
    struct signal *s = &g_sig[h.index];
    if (!s->used || s->self.index != h.index || s->self.generation != h.generation) return 0;
    return s;
}
uint32_t signal_generation(struct signal_handle h) {
    struct signal *s = signal_resolve(h);
    return s ? s->generation : 0;
}

static void signal_store(struct signal *s, const void *value, size_t size) {
    if (size <= SIGNAL_INLINE_SIZE) {
        if (s->heap_value) { free(s->heap_value); s->heap_value = 0; }
        memcpy(s->inline_value, value, size);
    } else {
        if (!s->heap_value || s->value_size != size) s->heap_value = realloc(s->heap_value, size);
        memcpy(s->heap_value, value, size);
    }
    s->value_size = size;
}
static const void *signal_current(struct signal *s) {
    return (s->value_size <= SIGNAL_INLINE_SIZE) ? (const void *)s->inline_value : s->heap_value;
}

struct signal_handle signal_create(const void *initial, size_t size) {
    uint32_t idx;
    if (g_sig_free_top >= 0) idx = g_sig_free[g_sig_free_top--];
    else if (g_sig_hw < SIGNAL_MAX) idx = g_sig_hw++;
    else { struct signal_handle nil = {0,0}; return nil; }

    struct signal *s = &g_sig[idx];
    uint32_t gen = s->self.generation; if (gen == 0) gen = 1;
    void *keep_heap = s->heap_value; (void)keep_heap;
    memset(s, 0, sizeof(*s));
    s->self.index = idx; s->self.generation = gen;
    s->used = true; s->generation = 0;
    if (initial && size) signal_store(s, initial, size);
    else s->value_size = size;
    return s->self;
}

void signal_get(struct signal_handle h, void *out, size_t size) {
    struct signal *s = signal_resolve(h);
    if (!s) return;
    if (out) {
        size_t n = size < s->value_size ? size : s->value_size;
        memcpy(out, signal_current(s), n);
    }
    /* read + track: register a dependency on the currently-running scope */
    extern void reactive_track_read(struct signal_handle);   /* defined below */
    reactive_track_read(h);
}

void signal_set(struct signal_handle h, const void *value, size_t size) {
    struct signal *s = signal_resolve(h);
    if (!s) return;

    if (size == s->value_size && s->value_size &&
        memcmp(signal_current(s), value, size) == 0)
        return;   /* no-op skip: identical bytes -> no notify, generation unchanged */

    signal_store(s, value, size);
    s->generation++;

    for (struct scope_edge *e = s->dependents; e; e = e->next_in_signal) {
        struct scope *sc = e->owning_scope;
        if (sc && !sc->dirty) {
            sc->dirty = true;
            if (g_dirty_count < DIRTY_WORKLIST_MAX)
                g_dirty_worklist[g_dirty_count++] = sc->self;   /* dedup via the dirty flag */
        }
    }
}

/* Remove one edge from BOTH lists (the scope's read-list and the signal's
 * dependents), then free it. O(1) thanks to the bidirectional links. */
static void edge_unlink_and_free(struct scope_edge *e) {
    /* from the signal's dependents list */
    if (e->prev_in_signal) e->prev_in_signal->next_in_signal = e->next_in_signal;
    else { struct signal *sig = signal_resolve(e->signal); if (sig) sig->dependents = e->next_in_signal; }
    if (e->next_in_signal) e->next_in_signal->prev_in_signal = e->prev_in_signal;
    /* from the scope's edge list */
    if (e->prev_in_scope) e->prev_in_scope->next_in_scope = e->next_in_scope;
    else if (e->owning_scope) e->owning_scope->edges = e->next_in_scope;
    if (e->next_in_scope) e->next_in_scope->prev_in_scope = e->prev_in_scope;
    edge_arena_free(e);
}

void signal_destroy(struct signal_handle h) {
    struct signal *s = signal_resolve(h);
    if (!s) return;
    /* drop every dependent edge so no scope keeps a dangling dependency */
    struct scope_edge *e = s->dependents;
    while (e) { struct scope_edge *next = e->next_in_signal; edge_unlink_and_free(e); e = next; }
    if (s->heap_value) free(s->heap_value);
    s->used = false;
    s->self.index = 0;
    s->self.generation++;                     /* ABA: invalidate outstanding handles */
    g_sig_free[++g_sig_free_top] = h.index;
}

/* ------------------------------------------------------------------------- */
/* scopes                                                                    */
/* ------------------------------------------------------------------------- */

struct scope *scope_resolve(struct scope_handle h) {
    if (h.index == 0 || h.index >= SCOPE_MAX) return 0;
    struct scope *s = &g_scope[h.index];
    if (!s->used || s->self.index != h.index || s->self.generation != h.generation) return 0;
    return s;
}
bool scope_is_dirty(struct scope_handle h) {
    struct scope *s = scope_resolve(h);
    return s && s->dirty;
}
uint32_t reactivity_dirty_count(void) { return g_dirty_count; }

struct scope_handle scope_create(void (*fn)(void *), void *ctx) {
    uint32_t idx;
    if (g_scope_free_top >= 0) idx = g_scope_free[g_scope_free_top--];
    else if (g_scope_hw < SCOPE_MAX) idx = g_scope_hw++;
    else { struct scope_handle nil = {0,0}; return nil; }

    struct scope *s = &g_scope[idx];
    uint32_t gen = s->self.generation; if (gen == 0) gen = 1;
    memset(s, 0, sizeof(*s));
    s->self.index = idx; s->self.generation = gen;
    s->used = true; s->fn = fn; s->ctx = ctx;
    return s->self;
}

/* clear ALL of a scope's edges (bidirectional). Step 1 of a re-run, and reused
 * by destroy. */
static void scope_clear_edges(struct scope *s) {
    struct scope_edge *e = s->edges;
    while (e) { struct scope_edge *next = e->next_in_scope; edge_unlink_and_free(e); e = next; }
    s->edges = 0;
}

void scope_destroy(struct scope_handle h) {
    struct scope *s = scope_resolve(h);
    if (!s) return;
    scope_clear_edges(s);
    s->used = false;
    s->self.index = 0;
    s->self.generation++;
    g_scope_free[++g_scope_free_top] = h.index;
}

/* Called from signal_get() -- attribute the read to the top of the tracking
 * stack. Non-static (signal_get references it via extern) so both live in this
 * one TU regardless of declaration order. */
void reactive_track_read(struct signal_handle sig_h) {
    if (g_scope_stack_top < 0) return;          /* untracked read -- fine */
    struct scope *s = g_scope_stack[g_scope_stack_top];
    struct signal *sig = signal_resolve(sig_h);
    if (!sig) return;

    struct scope_edge *e = edge_arena_alloc();
    if (!e) return;
    e->signal = sig_h;
    e->owning_scope = s;
    /* push onto the scope's read-list */
    e->next_in_scope = s->edges; e->prev_in_scope = 0;
    if (s->edges) s->edges->prev_in_scope = e;
    s->edges = e;
    /* push onto the signal's dependents list */
    e->next_in_signal = sig->dependents; e->prev_in_signal = 0;
    if (sig->dependents) sig->dependents->prev_in_signal = e;
    sig->dependents = e;
}

void scope_rerun(struct scope_handle h) {
    struct scope *s = scope_resolve(h);
    if (!s) return;                     /* stale handle (destroyed-while-dirty) */
    if (s->currently_running) return;   /* reentrancy guard */

    scope_clear_edges(s);               /* Step 1: clear stale deps FIRST */

    if (g_scope_stack_top + 1 >= SCOPE_STACK_MAX) return;
    g_scope_stack[++g_scope_stack_top] = s;   /* Step 2: push, run, pop */
    s->currently_running = true;
    if (s->fn) s->fn(s->ctx);           /* fresh signal_get()s re-track here */
    s->currently_running = false;
    g_scope_stack_top--;
    s->dirty = false;
}

bool signal_has_dependent(struct signal_handle sig_h, struct scope_handle sc_h) {
    struct signal *sig = signal_resolve(sig_h);
    if (!sig) return false;
    for (struct scope_edge *e = sig->dependents; e; e = e->next_in_signal) {
        if (e->owning_scope && e->owning_scope->self.index == sc_h.index &&
            e->owning_scope->self.generation == sc_h.generation)
            return true;
    }
    return false;
}

void reactivity_test_reset(void) { g_dirty_count = 0; g_scope_stack_top = -1; }

void reactivity_flush(void) {
    uint32_t n = g_dirty_count;          /* snapshot -- writes during flush defer */
    for (uint32_t i = 0; i < n; i++) {
        struct scope_handle h = g_dirty_worklist[i];
        struct scope *s = scope_resolve(h);
        if (s && s->dirty) scope_rerun(h);   /* stale handle -> resolve NULL, skip */
    }
    /* keep anything enqueued DURING this flush for the next one */
    uint32_t rem = g_dirty_count - n;
    for (uint32_t i = 0; i < rem; i++) g_dirty_worklist[i] = g_dirty_worklist[n + i];
    g_dirty_count = rem;
}
