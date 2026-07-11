#include "arch/x86_64/cpu/spinlock.h"


void spinlock_init(spinlock_t *lock) {
    lock->locked = 0;
    lock->saved_flags = 0;
}


void spin_lock(spinlock_t *lock) {
    // Disable interrupts and save previous state
    uint64_t flags;
    __asm__ volatile ("pushfq; pop %0" : "=r"(flags) :: "memory");

    __asm__ volatile ("cli" ::: "memory"); // disable interrupts on this core

    // Spin until we can set locked from 0 to 1
    // __atomic_exchange_n sets *locked to 1 and returns the OLD value
    // If the old value was 0, we acquired the lock. If it was 1, we keep spinning.

    while (__atomic_exchange_n(&lock->locked, 1, __ATOMIC_ACQUIRE) != 0) {
        // Spin. Use 'pause' instruction to reduce power consumption and improve performance on hyperthreaded CPUs
        __asm__ volatile ("pause" ::: "memory");
    }

    // We have the lock, save the previous interrupt state inside it so unlock can restore it
    // can restore them. (safe because only the thread that holds the lock can writes this)
    lock->saved_flags = flags;
}


void spin_unlock(spinlock_t *lock) {
    uint64_t flags = lock->saved_flags;

    //  Release the lock with release semantics (all our writes visible first)
    __atomic_store_n(&lock->locked, 0, __ATOMIC_RELEASE);

    // Restore previous interrupt state
    if (flags & (1ULL << 9)) { // Check IF flag in saved RFLAGS
        __asm__ volatile ("sti" ::: "memory"); // Re-enable interrupts if they were enabled before
    }
}
