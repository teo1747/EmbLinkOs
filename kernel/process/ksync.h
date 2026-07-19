#ifndef __KSYNC__H__
#define __KSYNC__H__

#include <stdint.h>
#include "include/types.h"     /* the kernel's freestanding bool (NOT <stdbool.h>) */
#include "process/process.h"   /* struct wait_queue, sched_lock, current_thread */

/* ==========================================================================
 * Sleeping synchronization: a counting SEMAPHORE and a MUTEX.
 *
 * WHY THESE EXIST, AND WHY THEY ARE NOT SPINLOCKS
 * -----------------------------------------------
 * The kernel already has spinlock_t (kernel/arch/x86_64/cpu/spinlock.h). A
 * spinlock is correct only when the holder CANNOT sleep while holding it: the
 * waiter burns a core spinning, so if the holder yields/blocks, the waiter can
 * deadlock the core (single-CPU) or waste it (SMP). That rules a spinlock OUT
 * for anything held across an operation that waits -- and disk I/O, the case
 * that motivated this, `hlt`-spins waiting for the drive (ata.c) while
 * preemptible.
 *
 * These primitives BLOCK instead: a contended waiter is taken off the run queue
 * and put to sleep on a wait_queue, so the core does other work until the
 * holder releases. That is the whole distinction -- spin vs sleep -- and it is
 * a property of the CALLER (does it sleep while holding?), not a preference.
 *
 * WHAT THEY ARE BUILT ON
 * ----------------------
 * Both are the wait_queue pattern that keyboard.c / pipe.c / block.c had each
 * open-coded: a bit of state plus a queue, with {test -> modify -> maybe-block}
 * made atomic by g_sched_lock (sched_lock()). g_sched_lock is the lock the
 * wait_queue_* calls require ANYWAY, so there is no second lock and no lock
 * ordering to get wrong. This just gives the pattern one correct home.
 *
 * PRE-SCHEDULER SAFE BY CONSTRUCTION
 * ----------------------------------
 * Before the scheduler starts, the kernel is single-threaded: the state check
 * always succeeds on the fast path, so sched_block_current_locked() -- which
 * needs a current_thread to block -- is never reached. current_thread being
 * NULL that early is therefore fine; no path dereferences it uncontended.
 *
 * CANCELLATION (docs/INTERRUPTION.md): the plain sem_wait()/mutex_lock() are
 * UNINTERRUPTIBLE -- a blocked acquire ignores the process's cancelled flag and
 * sleeps until the resource is genuinely released. That is the right default for
 * a lock a cleanup path must still be able to take, and for one held only
 * briefly. The *_interruptible() variants opt INTO cancellation: a cancelled
 * process abandons a blocking acquire with -EMBK_ECANCELED instead of sleeping
 * (matching every other blocking syscall). Use them when the wait can be long --
 * e.g. a lock guarding a disk operation -- so a ^C'd waiter unwinds rather than
 * stalling for the whole operation. Cancellation gates only BLOCKING: an
 * uncontended acquire still succeeds regardless of the flag (a completed
 * operation wins, as process_wait() returns a ready child's status first).
 * NOT recursive either way. See the notes on mutex_lock.
 * NOT IRQ-safe: never take one of these from an interrupt handler -- a handler
 * cannot sleep, so a contended acquire there would block a context that must
 * not block. (Spinlocks are the IRQ-context tool; that is their job.)
 * ========================================================================== */

/* --- counting semaphore --------------------------------------------------
 * `count` permits. sem_wait consumes one (blocking at zero); sem_post returns
 * one (waking a blocked waiter). A mutex is the count==1 special case, but the
 * semaphore does NOT track ownership, so it is also the right tool for
 * producer/consumer counting (N buffers, N slots) where "the releaser is not
 * the acquirer" is legal and expected.
 *
 * `struct semaphore` / `struct mutex` and their INIT macros are defined in
 * process.h (next to struct wait_queue), because struct process embeds a mutex
 * by value. This header owns the OPERATIONS. */

void sem_init(struct semaphore *s, int32_t initial);
void sem_wait(struct semaphore *s);      /* P: take a permit, block (uninterruptibly) if none */
int  sem_wait_interruptible(struct semaphore *s); /* P, but -EMBK_ECANCELED if cancelled while blocked; 0 on permit */
void sem_post(struct semaphore *s);      /* V: return a permit, wake one waiter */
int  sem_trywait(struct semaphore *s);   /* 1 if a permit was taken, 0 if none (never blocks) */

/* --- mutex ----------------------------------------------------------------
 * A mutex is a semaphore of one permit PLUS ownership. Ownership buys two
 * things a bare binary semaphore cannot give:
 *   - self-deadlock detection: locking a mutex you already hold is a bug that
 *     would otherwise hang forever; we catch it loudly (see mutex_lock).
 *   - a stated contract that the unlocker is the locker, which is what makes
 *     "mutex" mean mutual exclusion of a critical section rather than a counter.
 *
 * NON-RECURSIVE on purpose: a recursive mutex hides re-entrancy bugs (you stop
 * noticing that you re-entered a critical section). If you genuinely need to
 * re-enter, restructure so the lock is taken once at the boundary.
 *
 * (struct mutex + MUTEX_INIT are defined in process.h -- see the semaphore note
 * above.) */

void mutex_init(struct mutex *m);
void mutex_lock(struct mutex *m);        /* acquire, block (uninterruptibly) until free */
int  mutex_lock_interruptible(struct mutex *m); /* -EMBK_ECANCELED if cancelled while blocked; 0 on acquire */
void mutex_unlock(struct mutex *m);
int  mutex_trylock(struct mutex *m);     /* 1 if acquired, 0 if already held */

#endif /* __KSYNC__H__ */
