#ifndef __SPINLOCK_H__
#define __SPINLOCK_H__

#include "../include/types.h"
#include <stdint.h>



// A spinlock is just a flag plus saved interrupt state.

typedef struct spinlock {
    volatile uint32_t locked; // 0 = unlocked, 1 = locked
    uint64_t saved_flags;   // saved RFLAGS for restoring interrupt state on unlock
} spinlock_t;

// static initializer for spinlocks
#define SPINLOCK_INIT { 0, 0 }


// Initialize a spinlock (set locked to 0)
void spinlock_init(spinlock_t *lock);

// Acquire the spinlock. Disables interrupts and saves previous state in lock->saved_rflags.
// If the lock is already held, it will spin until it becomes available.
void spin_lock(spinlock_t *lock);

// Release the spinlock. Restores previous interrupt state from lock->saved_rflags.
void spin_unlock(spinlock_t *lock);



#endif /* __SPINLOCK_H__ */