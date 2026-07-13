#ifndef __EMBLINK_UI_SCOPE_H__
#define __EMBLINK_UI_SCOPE_H__

/* ui/reactive/scope.h -- EmbLink UI Piece 6: reactive scopes + dependency
 * edges + the batched flush. A `scope` is the ONE primitive for both Piece 7
 * component bodies and arbitrary effects -- no separate "effect" concept. */

#include "signal.h"

struct scope_handle { uint32_t index, generation; };

struct scope;   /* forward */

/* An edge is doubly-linked in BOTH directions so clearing a scope's stale
 * dependencies (scope_rerun step 1) is O(1) removal per edge from each of the
 * two lists it belongs to -- singly-linked would be O(n^2) total. */
struct scope_edge {
    struct signal_handle signal;
    struct scope        *owning_scope;
    struct scope_edge *prev_in_scope,  *next_in_scope;    /* this scope's read-list */
    struct scope_edge *prev_in_signal, *next_in_signal;   /* that signal's dependents */
};

struct scope {
    struct scope_handle self;
    void (*fn)(void *ctx);
    void *ctx;
    struct scope_edge *edges;      /* head of THIS scope's current dependency list */
    bool dirty;
    bool currently_running;         /* reentrancy guard */
    bool used;
};

struct scope_handle scope_create(void (*fn)(void *ctx), void *ctx);
struct scope *scope_resolve(struct scope_handle h);   /* NULL if stale */
void scope_destroy(struct scope_handle h);

/* Re-run a scope: clear its old edges (bidirectional), push it as the tracking
 * scope, run its fn (fresh signal_get()s re-track), pop, clear dirty. */
void scope_rerun(struct scope_handle h);

/* Frame-boundary entry point: re-run every dirty scope enqueued since the last
 * flush. Snapshotted length -> a write DURING flush defers to the next flush,
 * not a recursive same-frame cascade. Stale (destroyed) handles skip safely. */
void reactivity_flush(void);

/* --- test-facing --- */
bool     scope_is_dirty(struct scope_handle h);
uint32_t reactivity_dirty_count(void);
bool     signal_has_dependent(struct signal_handle sig, struct scope_handle sc);
void     reactivity_test_reset(void);   /* drop the dirty worklist + tracking stack */

#endif /* __EMBLINK_UI_SCOPE_H__ */
