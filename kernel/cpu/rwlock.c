#include "rwlock.h"
#include "../include/kprintf.h"

// RFLAGS.IF (interrupt-enable) is bit 9. We re-enable interrupts on release
// only if they were enabled when the lock was taken — same rule as spinlock.c.
#define RFLAGS_IF (1ULL << 9)

static inline uint64_t save_flags_cli(void) {
    uint64_t flags;
    __asm__ volatile ("pushfq; pop %0" : "=r"(flags) :: "memory");
    __asm__ volatile ("cli" ::: "memory");
    return flags;
}

static inline void restore_flags(uint64_t flags) {
    if (flags & RFLAGS_IF) {
        __asm__ volatile ("sti" ::: "memory");
    }
}

void rwlock_init(rwlock_t *lock) {
    lock->state = 0;
}

uint64_t read_lock(rwlock_t *lock) {
    uint64_t flags = save_flags_cli();

    // Take a reader reference: bump the count, but only from a non-writer state
    // (state >= 0). A negative state means a writer holds the lock, so we spin.
    int32_t expected = __atomic_load_n(&lock->state, __ATOMIC_RELAXED);
    for (;;) {
        if (expected < 0) {                       // writer holds it; wait
            __asm__ volatile ("pause" ::: "memory");
            expected = __atomic_load_n(&lock->state, __ATOMIC_RELAXED);
            continue;
        }
        // ACQUIRE on success so reads of the protected data can't be hoisted
        // above the lock acquisition.
        if (__atomic_compare_exchange_n(&lock->state, &expected, expected + 1,
                                        false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            break;
        }
        // CAS failed: `expected` was reloaded with the current value; retry.
    }

    return flags;
}

void read_unlock(rwlock_t *lock, uint64_t flags) {
    // RELEASE so the protected reads complete before the count drops, making
    // room for a writer that may then observe state == 0.
    __atomic_fetch_sub(&lock->state, 1, __ATOMIC_RELEASE);
    restore_flags(flags);
}

uint64_t write_lock(rwlock_t *lock) {
    uint64_t flags = save_flags_cli();

    // Exclusive access: claim the lock only from the fully-free state (0 -> -1).
    int32_t expected = 0;
    while (!__atomic_compare_exchange_n(&lock->state, &expected, -1,
                                        false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        __asm__ volatile ("pause" ::: "memory");
        expected = 0;   // only ever transition from the free state
    }

    return flags;
}

void write_unlock(rwlock_t *lock, uint64_t flags) {
    // RELEASE so all writes under the lock are visible before it opens.
    __atomic_store_n(&lock->state, 0, __ATOMIC_RELEASE);
    restore_flags(flags);
}

// --- self-test --------------------------------------------------------------
// Single-core, so this can only check state transitions, not real contention.
// It verifies the encoding (readers add, writer is -1, both return to 0) and
// that nested readers stack correctly.

#define RW_CHECK(cond, msg)                                       \
    do {                                                          \
        if (!(cond)) {                                            \
            kprintf("rwlock selftest FAIL: %s\n", (msg));         \
            return -1;                                            \
        }                                                         \
    } while (0)

int rwlock_run_selftests(void) {
    rwlock_t l = RWLOCK_INIT;
    RW_CHECK(l.state == 0, "init state not 0");

    // Single reader: state -> 1 -> 0.
    uint64_t f1 = read_lock(&l);
    RW_CHECK(l.state == 1, "one reader should give state 1");

    // Nested second reader (allowed: shared access): state -> 2.
    uint64_t f2 = read_lock(&l);
    RW_CHECK(l.state == 2, "two readers should give state 2");

    read_unlock(&l, f2);
    RW_CHECK(l.state == 1, "after one release state should be 1");
    read_unlock(&l, f1);
    RW_CHECK(l.state == 0, "after both releases state should be 0");

    // Writer: state -> -1 -> 0.
    uint64_t fw = write_lock(&l);
    RW_CHECK(l.state == -1, "writer should give state -1");
    write_unlock(&l, fw);
    RW_CHECK(l.state == 0, "after writer release state should be 0");

    // Reader after writer: lock is reusable.
    uint64_t f3 = read_lock(&l);
    RW_CHECK(l.state == 1, "reader after writer should give state 1");
    read_unlock(&l, f3);
    RW_CHECK(l.state == 0, "final state should be 0");

    kprintf("rwlock selftest: OK (reader stacking, writer exclusivity, reuse)\n");
    return 0;
}
