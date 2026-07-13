#ifndef __EMBLINK_UI_SIGNAL_H__
#define __EMBLINK_UI_SIGNAL_H__

/* ui/reactive/signal.h -- EmbLink UI Piece 6: reactive signals.
 *
 * A general reactive-value system: knows NOTHING about UI, windows, or scene
 * trees. It answers exactly one question: when does something need to be
 * recomputed. What recomputing does (Piece 7's diffing + Piece 3/5 mutation)
 * is layered entirely on top.
 *
 * Threading: the tracking stack (scope.h) is per-process, NOT synchronized --
 * a client's UI logic runs on one thread, the same "main thread" assumption
 * UIKit/AppKit/Android all make. */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define SIGNAL_INLINE_SIZE 16

struct signal_handle { uint32_t index, generation; };

struct scope_edge;   /* forward -- signal only holds a pointer to the list head */

struct signal {
    struct signal_handle self;              /* slot identity (ABA guard) */
    uint8_t  inline_value[SIGNAL_INLINE_SIZE];
    void    *heap_value;                     /* non-NULL if value_size > INLINE */
    size_t   value_size;

    struct scope_edge *dependents;           /* head of this signal's dependent-scope edges */
    uint32_t generation;                      /* bumped on every ACTUAL (non-no-op) write */
    bool     used;
};

/* Create a signal holding a copy of `initial` (`size` bytes). */
struct signal_handle signal_create(const void *initial, size_t size);

/* Read the value into `out` AND, if a scope is currently tracking (scope.h),
 * register a dependency edge -- this dual read+track is what makes dependency
 * tracking automatic instead of manually declared. */
void signal_get(struct signal_handle h, void *out, size_t size);

/* Write. No-op skip: if the new bytes byte-equal the current value, returns
 * immediately -- no dependents notified, generation unchanged. Otherwise marks
 * every dependent scope dirty + enqueues it (batched; nothing re-runs here). */
void signal_set(struct signal_handle h, const void *value, size_t size);

void signal_destroy(struct signal_handle h);

/* --- internal / test-facing --- */
struct signal *signal_resolve(struct signal_handle h);   /* NULL if stale */
uint32_t signal_generation(struct signal_handle h);      /* value generation (T3) */

#endif /* __EMBLINK_UI_SIGNAL_H__ */
