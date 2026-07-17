#include "process/ksync.h"
#include "include/kprintf.h"

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

void sem_wait(struct semaphore *s) {
    sched_lock();
    /* WHILE, not if: a woken waiter must RE-CHECK. sem_post wakes one thread but
     * releases the lock before that thread runs, so a third thread can slip in
     * and take the permit first (a "spurious"-looking wake). The loop re-tests
     * count under the lock and sleeps again if it lost the race. This is the
     * standard mesa-monitor discipline; getting it wrong (an `if`) is the
     * classic lost-wakeup / stolen-permit bug. */
    while (s->count == 0) {
        sched_block_current_locked(&s->wq);   /* returns with the lock RELEASED */
        sched_lock();                          /* re-acquire, then re-test */
    }
    s->count--;
    sched_unlock();
}

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

void mutex_lock(struct mutex *m) {
    sched_lock();

    /* Self-deadlock: locking a mutex this thread already holds. Without the
     * check the thread would block on itself and never wake -- a silent hang
     * that a `test <x>` would report only as "the OS stopped". Catch it while
     * we still have a stack to name. current_thread can be NULL pre-scheduler,
     * but so is owner if a lock leaked from that phase, and a re-lock there is
     * equally a single-threaded self-deadlock -- so the NULL==NULL match is the
     * correct verdict, not a false positive. */
    if (m->locked && m->owner == current_thread) {
        kprintf("KSYNC PANIC: mutex %p re-locked by its owner (self-deadlock)\n",
                (void *)m);
        for (;;) __asm__ volatile ("cli; hlt");   /* a hang here beats a hang nowhere */
    }

    while (m->locked) {
        sched_block_current_locked(&m->wq);   /* returns with the lock RELEASED */
        sched_lock();                          /* re-acquire, then re-test */
    }
    m->locked = true;
    m->owner  = current_thread;
    sched_unlock();
}

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
