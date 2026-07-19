#include "process/ksync.h"
#include "include/kprintf.h"
#include "include/errno.h"   /* EMBK_ECANCELED for the interruptible acquires */

/* All operations serialize under g_sched_lock (sched_lock()). It is the SAME
 * lock sched_block_current_locked() and wait_queue_wake_one() require, so
 * holding it across {test the state -> maybe block/wake} is exactly what makes
 * those calls legal AND makes the check-then-act atomic. No second lock exists,
 * so there is no ordering to reason about. */

/* ==== semaphore ========================================================== */

void sem_init(struct semaphore *s, int32_t initial) {
    s->count   = initial;
    s->wq.head = 0;
}

/* Shared acquire for both entry points. `interruptible` decides whether a
 * cancelled process is allowed to abandon a BLOCKING wait (see the note in the
 * loop); everything else -- the mesa-monitor while-loop, the permit decrement --
 * is identical, so the two variants can never drift apart. */
static int sem_acquire(struct semaphore *s, bool interruptible) {
    sched_lock();
    /* WHILE, not if: a woken waiter must RE-CHECK. sem_post wakes one thread but
     * releases the lock before that thread runs, so a third thread can slip in
     * and take the permit first (a "spurious"-looking wake). The loop re-tests
     * count under the lock and sleeps again if it lost the race. This is the
     * standard mesa-monitor discipline; getting it wrong (an `if`) is the
     * classic lost-wakeup / stolen-permit bug. */
    while (s->count == 0) {
        /* Cancellation-aware acquire (sem_wait_interruptible only): if this
         * process was cancelled, return -ECANCELED rather than sleep until some
         * poster happens to arrive. Checked HERE, only when we WOULD block: an
         * available permit is taken on the fast path regardless of the flag,
         * because a completed (uncontended) acquire wins over cancellation --
         * the same rule process_wait() follows by returning a ready zombie's
         * status before it ever considers the flag. current_process is NULL
         * pre-scheduler, but this contended path is unreachable then anyway. */
        if (interruptible && current_process && current_process->cancelled) {
            sched_unlock();
            return -EMBK_ECANCELED;
        }
        sched_block_current_locked(&s->wq);   /* returns with the lock RELEASED */
        sched_lock();                          /* re-acquire, then re-test */
    }
    s->count--;
    sched_unlock();
    return EMBK_OK;
}

void sem_wait(struct semaphore *s) { (void)sem_acquire(s, false); }

int sem_wait_interruptible(struct semaphore *s) { return sem_acquire(s, true); }

void sem_post(struct semaphore *s) {
    sched_lock();
    s->count++;
    /* Wake ONE: one returned permit satisfies at most one waiter. Waking all
     * would make every waiter race for a single permit and all but one sleep
     * again -- a thundering herd for no benefit. */
    wait_queue_wake_one(&s->wq);
    sched_unlock();
}

int sem_trywait(struct semaphore *s) {
    int got = 0;
    sched_lock();
    if (s->count > 0) { s->count--; got = 1; }
    sched_unlock();
    return got;
}

/* ==== mutex ============================================================== */

void mutex_init(struct mutex *m) {
    m->locked  = false;
    m->owner   = 0;
    m->wq.head = 0;
}

/* Shared acquire for both entry points; `interruptible` gates only the abandon-
 * on-cancel path in the blocking loop. The self-deadlock check and the ownership
 * stamp are identical either way, so keeping them in one place stops the two
 * variants from drifting. */
static int mutex_acquire(struct mutex *m, bool interruptible) {
    sched_lock();

    /* Self-deadlock: locking a mutex this thread already holds. Without the
     * check the thread would block on itself and never wake -- a silent hang
     * that a `test <x>` would report only as "the OS stopped". Catch it while
     * we still have a stack to name. current_thread can be NULL pre-scheduler,
     * but so is owner if a lock leaked from that phase, and a re-lock there is
     * equally a single-threaded self-deadlock -- so the NULL==NULL match is the
     * correct verdict, not a false positive. (Cancellation does not exempt this:
     * a thread waiting on ITSELF is a bug the flag cannot rescue -- only the
     * holder, which is us, could release, and we are stuck here.) */
    if (m->locked && m->owner == current_thread) {
        kprintf("KSYNC PANIC: mutex %p re-locked by its owner (self-deadlock)\n",
                (void *)m);
        for (;;) __asm__ volatile ("cli; hlt");   /* a hang here beats a hang nowhere */
    }

    while (m->locked) {
        /* Cancellation-aware acquire (mutex_lock_interruptible only): a cancelled
         * process refuses to BLOCK and returns -ECANCELED, so a waiter stuck
         * behind a long-held lock (e.g. one guarding a disk wait) can unwind
         * instead of sleeping for the whole operation. Checked only on the
         * would-block path: a FREE mutex is taken above regardless of the flag
         * (completed-op-wins, as process_wait() returns a ready child's status
         * before considering cancellation). The plain mutex_lock() passes
         * interruptible=false and so keeps its unconditional acquire. */
        if (interruptible && current_process && current_process->cancelled) {
            sched_unlock();
            return -EMBK_ECANCELED;
        }
        sched_block_current_locked(&m->wq);   /* returns with the lock RELEASED,
                                               * IF re-enabled (see its comment) */
        sched_lock();                          /* re-acquire, then re-test */
    }
    m->locked = true;
    m->owner  = current_thread;
    sched_unlock();
    return EMBK_OK;
}

void mutex_lock(struct mutex *m) { (void)mutex_acquire(m, false); }

int mutex_lock_interruptible(struct mutex *m) { return mutex_acquire(m, true); }

void mutex_unlock(struct mutex *m) {
    sched_lock();

    /* Unlocking someone else's (or a free) mutex is a bug on par with a bad
     * free(): the critical section it was guarding is now unprotected and the
     * owner does not know. Report it and do NOT wake a waiter -- handing the
     * lock to a waiter on a bogus unlock would compound the error. */
    if (!m->locked || m->owner != current_thread) {
        kprintf("KSYNC WARN: mutex %p unlock by non-owner (locked=%d owner=%p me=%p)\n",
                (void *)m, m->locked, (void *)m->owner, (void *)current_thread);
        sched_unlock();
        return;
    }

    m->locked = false;
    m->owner  = 0;
    wait_queue_wake_one(&m->wq);   /* the woken thread re-tests m->locked */
    sched_unlock();
}

int mutex_trylock(struct mutex *m) {
    int got = 0;
    sched_lock();
    if (!m->locked) {
        m->locked = true;
        m->owner  = current_thread;
        got = 1;
    }
    sched_unlock();
    return got;
}
