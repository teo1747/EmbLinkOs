#ifndef __RWLOCK_H__
#define __RWLOCK_H__

#include "../include/types.h"
#include <stdint.h>

// A reader-writer lock: many concurrent readers OR one exclusive writer.
// Use this for read-mostly data where readers dominate and should not
// serialize against each other the way the plain spinlock_t forces them to.
//
// `state` encodes the whole lock in one atomic word:
//      0   unlocked
//     >0   that many readers hold it (shared)
//     -1   one writer holds it (exclusive)
//
// INTERRUPTS: like spinlock_t, the lock is taken with interrupts disabled on
// this core, so a holder cannot be interrupted into code that takes the same
// lock and self-deadlocks. But spinlock_t stashes the saved RFLAGS *inside the
// lock* because it has exactly one holder. A reader-writer lock can have many
// readers at once, so there is no single slot to stash flags in — each caller
// saves its own interrupt state and hands it back on release. That is why the
// lock calls RETURN the flags and the unlock calls take them as an argument
// (the classic irqsave / irqrestore pattern).
//
// FAIRNESS: this is a simple reader-preferring lock. A continuous stream of
// readers can starve a writer waiting for the count to hit zero. That is fine
// for read-mostly data; if writers must make progress under sustained read
// load, this needs a writer-preferring variant (e.g. a pending-writer flag
// that blocks new readers). Noted, not built.

typedef struct rwlock {
    volatile int32_t state;   // 0 = free, >0 = reader count, -1 = writer
} rwlock_t;

// Static initializer for an unlocked rwlock: rwlock_t l = RWLOCK_INIT;
#define RWLOCK_INIT { 0 }

// Initialize an rwlock to the unlocked state.
void rwlock_init(rwlock_t *lock);

// Acquire shared (read) access. Multiple readers proceed concurrently; blocks
// while a writer holds the lock. Returns the saved interrupt state, which MUST
// be passed back to read_unlock.
uint64_t read_lock(rwlock_t *lock);

// Release shared access. `flags` is the value returned by the matching
// read_lock call.
void read_unlock(rwlock_t *lock, uint64_t flags);

// Acquire exclusive (write) access. Blocks until no readers and no writer
// remain. Returns the saved interrupt state, which MUST be passed back to
// write_unlock.
uint64_t write_lock(rwlock_t *lock);

// Release exclusive access. `flags` is the value returned by the matching
// write_lock call.
void write_unlock(rwlock_t *lock, uint64_t flags);

// Single-core smoke test of the lock's state transitions. Returns 0 on success,
// negative on the first failed assertion.
int rwlock_run_selftests(void);

#endif /* __RWLOCK_H__ */
