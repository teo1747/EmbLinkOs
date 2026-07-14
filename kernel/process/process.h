#ifndef __PROCESS_H__
#define __PROCESS_H__

#include <stdint.h>
#include "include/types.h"
#include "arch/x86_64/cpu/kcontext.h"
#include "arch/x86_64/cpu/percpu.h"
#include "fs/fd.h"
#include "process/spawn.h"
#include "ipc/handle.h"    /* struct obj_handle + OBJ_HANDLE_MAX + the typed
                            * object-handle table (handle.h forward-declares
                            * struct process, so no cycle) */

/* Raised from 16/16 once ring-3 processes could genuinely have more than one
 * thread each (Phase 5, below) — 16 was sized for "one process, one thread"
 * plus a handful of kthreads, and stopped being enough headroom the moment a
 * single multithreaded process could plausibly want more than a couple of
 * threads. Still static arrays, not a dynamic allocator (§6.3's "revisit
 * only if the ceiling is actually hit" trade-off still applies at this new
 * size) — just a bigger fixed ceiling. MAX_THREADS > MAX_PROCESSES on
 * purpose: every process needs at least one thread, and multithreaded
 * processes need more than one, so the thread table has to outgrow the
 * process table, not match it 1:1. */
#define MAX_PROCESSES 64
#define MAX_THREADS   256

/* Was `(MAX_PROCESSES * 1024)` — a COINCIDENTAL formula (16 * 1024 = 16 KiB)
 * that happened to read as intentional but was never actually derived from
 * the process count; raising MAX_PROCESSES without noticing this coupling
 * would have silently quadrupled every kernel stack's size (to 64 KiB) for
 * no reason connected to why MAX_PROCESSES was raised. Written out
 * explicitly now so the two constants can never drift against each other
 * again. */
#define KSTACK_SIZE (16 * 1024)             // 16 KiB per thread's kernel stack

/* Same "big slot" VA scheme as USER_CODE_VA (0x400000000000)/USER_STACK_VA
 * (0x700000000000) in process.c -- slot 6, between code and stack. Was
 * missing four hex digits (0x60000000, a tiny low address) before, which
 * also broke USER_HEAP_VA_MAX's bound check for every real sbrk() call. */
#define USER_HEAP_VA_BASE 0x0000600000000000ULL
#define USER_HEAP_VA_MAX  (USER_HEAP_VA_BASE + 0x40000000ULL)   // 1 GiB of user heap space

enum process_state {
    PROCESS_UNUSED = 0,      /**< Process slot is unused */
    PROCESS_READY,            /**< Process is ready to run */
    PROCESS_RUNNING,          /**< Process is currently running */
    PROCESS_BLOCKED,          /**< Process is blocked/waiting */
    PROCESS_ZOMBIE            /**< Process has terminated but not yet cleaned up */
};

/* Fixed priority bands (docs/architecture/process-and-scheduling.md §4.2
 * Phase C): 0 is highest. schedule() always picks from the lowest-numbered
 * non-empty band first, round-robin within a band; PRIORITY_AGE_TICKS is
 * how long a READY-but-unpicked process waits before getting bumped up ONE
 * band, so a busy high band can never starve a low one forever.
 *
 * Deliberately short (200ms/band, not seconds): a process only competes
 * with the next band up once it's actually IN that band, so recovering
 * from PRIORITY_BACKGROUND all the way to PRIORITY_REALTIME contention
 * against a single non-yielding busy-looping process takes up to
 * SCHED_PRIORITY_BANDS-1 full aging periods in the worst case. At the
 * obvious "just use a round number of seconds" value that worst case is
 * several seconds of total unresponsiveness -- long enough that even the
 * kernel's own interactive shell (a plain PRIORITY_NORMAL process, see
 * main.c's process_adopt_current() call) would visibly freeze if a
 * spawned child ever ran busier and higher-priority than it. 200ms/band
 * bounds the worst case (BACKGROUND to REALTIME) to well under a second,
 * which is the same "bounded, not eliminated" tradeoff already accepted
 * elsewhere in this scheduler (e.g. preemption bounding an unresponsive
 * process to "at most one quantum" rather than promising zero delay). */
#define SCHED_PRIORITY_BANDS 4
#define PRIORITY_REALTIME    0
#define PRIORITY_INTERACTIVE 1
#define PRIORITY_NORMAL      2
#define PRIORITY_BACKGROUND  3
#define PRIORITY_AGE_TICKS   20    /* ~200ms per band at the 100Hz LAPIC tick */

/* Per-process table of process handles (docs/ARCHITECTURE.md §3.4/§3.5:
 * typed, capability-style handles for structural objects instead of a
 * raw ambient pid). A handle is a small integer meaningful only to the
 * process that holds it -- sys_spawn returns one, sys_wait/sys_kill take
 * one, and it's translated to the real pid through this table so a ring-3
 * process can only name a process it was actually handed a handle to. */
/* Headroom for how many children a process can hold handles to at once. A
 * spawn-and-forget parent (one that never wait()s its dead children) leaks a
 * slot per dead child; process_handle_reap_dead() reclaims those on pressure so
 * a full table self-heals instead of sys_spawn having to kill its new child --
 * this cap is therefore the limit on simultaneously-LIVE children, not a leak
 * ceiling. 32 gives a launcher comfortable headroom (256 B/process). */
#define PROC_HANDLE_MAX 32
struct proc_handle {
    uint32_t pid;
    bool used;
};

/* Intrusive wait queue: blocked THREADS are linked through their own
 * thread::wait_next field (see docs/architecture/process-and-scheduling.md
 * §7.3), so no separate allocation is needed to queue one. Threads, not
 * processes, are what actually blocks/wakes/gets dispatched -- a process is
 * a resource owner (address space, fds, handles), a thread is the
 * schedulable unit (docs/architecture/process-and-scheduling.md's Phase 4
 * split, §4.1). */
struct wait_queue {
    struct thread *head;
};

/* The schedulable unit. Everything schedule_locked() reads/writes on every
 * tick lives here: saved context, state, priority, which core (if any) it's
 * running/pinned to. `proc` is the resource owner it borrows an address
 * space and fd/handle tables from -- never NULL once a thread is allocated.
 *
 * Deliberately NOT holding pid: pid identifies a PROCESS (docs/architecture/
 * process-and-scheduling.md's Phase 4 rationale -- wait()/kill() name a
 * whole process, not one of its threads, since this phase doesn't expose a
 * ring-3 thread-kill/thread-wait surface). A thread that needs its owning
 * process's pid reads it via `proc->pid`. */
struct thread {
    struct kcontext ctx;       /**< Saved kernel context for this thread */

    /* x87 FPU + MXCSR + XMM0-15 state (the FXSAVE/FXRSTOR image, kernel/cpu/
     * kcontext.asm's kernel_ctx_switch), saved/restored around every context
     * switch alongside `ctx` above -- eager, unconditional, every thread
     * (kthread or ring-3), same simplicity tradeoff the GP-register save
     * already makes. The `aligned(16)` is load-bearing, not decorative:
     * FXSAVE/FXRSTOR #GP fault on an unaligned operand. thread_init_for()
     * (process.c) seeds this with a valid default state (zeroed + MXCSR =
     * 0x1F80, all exceptions masked) for every freshly created thread --
     * without that, a brand-new thread's first-ever FXRSTOR would load an
     * all-zero MXCSR (every exception UNmasked) and likely #XM on its very
     * first floating-point op. */
    unsigned char fpu_state[512] __attribute__((aligned(16)));
    uint64_t kstack_top;       /**< Virtual address of the top of this thread's kernel stack */
    uint64_t entry_point;      /**< Ring-3 user entry (process_trampoline) OR the real
                                 *   kthread function (kthread_trampoline stashes it here) */
    uint64_t user_rsp;         /**< User mode stack pointer (ring-3 threads only) */
    enum process_state state;  /**< Current state of this thread */

    /* Wait-queue membership. wait_queue is NULL unless state == BLOCKED;
     * process_kill() uses it to unlink a killed thread from whatever queue
     * it's blocked on, so a later wake can't walk into a dead TCB. */
    struct thread *wait_next;
    struct wait_queue *wait_queue;

    /* Which core (cpu_table[] index, kernel/cpu/percpu.h) this thread is
     * PROCESS_RUNNING on, set every time schedule()/process_start_first()
     * transitions it to RUNNING. -1 when not running anywhere (READY,
     * BLOCKED, ZOMBIE, or UNUSED). Needed the instant a second core
     * exists: process_kill()'s old two-way "is it current_thread or
     * not" check conflated "running on ME" with "running anywhere" --
     * once current_thread is per-CPU (docs/architecture/
     * process-and-scheduling.md's SMP phase), those are different
     * questions, and killing a thread running on a DIFFERENT core must
     * NOT reap it synchronously (nothing guarantees it isn't still
     * executing there this instant) -- see process_kill()'s comment. */
    int running_cpu;

    /* -1 = runnable on any core (every normal thread). >= 0 = ONLY that
     * cpu_table[] index may ever dispatch this thread --
     * schedule_locked()'s candidate scan skips it on every other core.
     * Set exclusively by process_adopt_current(): each core's adopted
     * bootstrap context (the BSP's shell, each AP's ap_main idle loop) is
     * pinned to the core that created it. This is not an optimization
     * knob but a LIVENESS guarantee the SMP exit path depends on: a core
     * whose current thread just died (ZOMBIE) must ALWAYS have something
     * else to switch to, or it stays physically executing on the dying
     * thread's kernel stack while the woken parent -- running in
     * parallel on another core -- reaps that exact stack out from under
     * it (observed as a double fault inside vmm_switch_address_space:
     * the CR3 reload flushed the stale TLB entries that were the only
     * thing still making the freed stack readable, the next stack access
     * page-faulted, and the #PF push onto that same dead stack escalated
     * to #DF). Pinning each core's own idle context makes "this core's
     * idle is READY and only I can take it" an invariant, so the dying
     * thread's core is guaranteed to switch off the doomed stack inside
     * the SAME g_sched_lock critical section that posted the zombie
     * hand-off -- and since the parent's reap also takes g_sched_lock,
     * the parent provably cannot even LOOK at the zombie until the dying
     * core is fully off its stack. */
    int pinned_cpu;

    /* Priority scheduling (docs/architecture/process-and-scheduling.md
     * §4.2 Phase C / §6.2). priority: 0 = highest band (see the
     * PRIORITY_* constants above); every new thread starts at
     * PRIORITY_NORMAL. ticks_since_scheduled counts how many timer ticks
     * this thread has gone without actually running while READY; once it
     * crosses PRIORITY_AGE_TICKS, schedule() bumps it up one band (floor
     * 0) and resets the counter, so a busy high band can't starve a lower
     * one forever. */
    uint8_t  priority;
    uint32_t ticks_since_scheduled;

    struct process *proc;         /* owning process -- never NULL */
    struct thread *proc_thread_next; /* intrusive link in proc->thread_list */

    /* Phase 5 (ring-3 threads, docs/architecture/process-and-scheduling.md):
     * everything below is meaningful only for a thread created via
     * thread_create_user() -- always false/0 for the process's own main
     * thread and for every ring-0 kthread (thread_create()/
     * process_create_kthread()), which keep the original "fire and forget,
     * auto-reap on exit" behavior unchanged. */

    /* True only for a thread created via thread_create_user(). A joinable
     * thread is NOT auto-reaped when it exits (schedule_locked()'s deferred-
     * reap step skips it, see process.c) -- it sits as a PROCESS_ZOMBIE,
     * exactly like an unwaited process zombie, until thread_join() collects
     * its exit_code and reaps it explicitly. Kthreads stay `false`: nothing
     * in this kernel joins a kthread today, so leaving them auto-reaped
     * (the pre-Phase-5 behavior) avoids leaking a slot forever waiting for
     * a join call that will never come. */
    bool joinable;

    /* This thread's OWN exit code (thread_exit_self()'s argument), distinct
     * from struct process::exit_code -- which is the WHOLE PROCESS's exit
     * code, last-writer-wins across every thread that ever calls
     * thread_exit_self() (see that function's comment). A sibling's
     * thread_join(tid) reads THIS field, not the process's. */
    int exit_code;

    /* The value placed in RDI at this thread's very first instruction
     * (process_trampoline() loads it just before the iretq) -- the
     * ring-3-visible "thread argument", mirroring the pthread_create()
     * `void *arg` shape even though this kernel has no libc/pthread of its
     * own. Always 0 for the process's own main thread and for kthreads
     * (neither passes one), so loading it unconditionally in the
     * trampoline is harmless for them. */
    uint64_t user_arg;

    uint64_t argc;          /* number of argv[] entries (user-mode threads only) */
    uint64_t argv_uva;          /* pointer to argv[] array (user-mode threads only) */
    bool has_argv;          /* true if this thread has a valid argc/argv_uva (user-mode threads only) */
};

/* The resource owner: address space, pid, parent/child tracking, fd/handle
 * tables. Everything here is shared by every thread of this process --
 * exactly the split docs/architecture/process-and-scheduling.md's §4.1
 * deferred until multiple threads were actually needed. A process with
 * `live_thread_count == 0` is a free slot (pid == 0 marks this, mirroring
 * how a fresh/reaped thread slot is marked PROCESS_UNUSED). */
struct process {
    uint32_t pid;              /**< Process ID. 0 means this slot is free. */
    uint64_t pml4_phys;        /**< Physical address of this process's PML4 (page table) */

    /* Parent/child tracking for a real blocking sys_wait/process_wait(). */
    struct process *parent;    /* who called spawn() to create us. NULL means
                                * "nobody will ever wait() on this pid" --
                                * on exit we fall straight to auto-reap
                                * instead of waiting to be collected. */
    uint32_t parent_pid;       /* parent's pid at the moment `parent` was set,
                                * NOT re-read from `parent->pid` later. `parent`
                                * is a raw pointer into the static process
                                * table, and slots get reused after reaping --
                                * without this, a dead parent's slot getting
                                * recycled by an unrelated new process would
                                * make `parent->pid != 0` look true again and
                                * silently hand this process's exit off to a
                                * stranger. Every parent-liveness check must
                                * compare parent->pid == parent_pid too, not
                                * just "parent is a non-free slot". */

    /* Intrusive list of this process's OWN currently-live (non-zombie)
     * children, head = child_list, linked through each child's child_next.
     * Not consulted by exit/reap/wait at all today -- populated purely so
     * `ps` can print a parent/child tree. Kept deliberately this simple:
     * no orphan-reparenting when a parent exits first (a child whose
     * parent has already gone just falls to auto-reap on its own exit,
     * same as any other parentless process -- see the `parent` field
     * above), because nothing in this OS needs Unix's re-parent-to-init-1
     * behavior yet. */
    struct process *child_list;
    struct process *child_next;

    /* Exited-but-not-yet-wait()'d children of THIS process, if this
     * process is acting as a parent. This is a SEPARATE field from
     * zombie_next below on purpose: a process can simultaneously be (a) a
     * zombie sitting in ITS OWN parent's list (using its own zombie_next
     * as the sibling link) and (b) the parent of some other still-running
     * process that later exits and needs a list head to attach to. Reusing
     * one field for both roles would let (b) silently clobber (a)'s
     * sibling-chain pointer the moment a grandchild outlives its own
     * parent's reaping -- a real, if rare, corruption case in any process
     * tree deeper than one level. */
    struct process *zombie_head;

    /* If I am currently linked as an exited child in some parent's
     * zombie_head list: pointer to the NEXT already-exited sibling after
     * me in that same list. NULL if I'm not currently linked into anyone's
     * zombie list (not a zombie, no live parent, or already wait()'d and
     * reaped). */
    struct process *zombie_next;

    struct wait_queue child_wait; /* wait queue for parent to block on until a child exits (zombie) */

    /* Phase 5 (ring-3 threads): wait queue woken every time ANY thread of
     * THIS process exits (thread_zombie_locked() wakes it unconditionally,
     * harmless no-op if nobody's blocked on it) -- thread_join()'s callers
     * block here and re-check their specific tid, the same "block on a
     * shared queue, re-check on wake" shape child_wait already uses for
     * process_wait(), just scoped to one process's own threads instead of
     * one process's own children. */
    struct wait_queue join_wait;

    int exit_code;             /**< Exit code once every thread has exited (meaningful only then) */

    /* How many of this process's threads are still schedulable (anything
     * but UNUSED). The process itself becomes a candidate for the
     * parent-hand-off/auto-reap decision only once this hits 0 -- see
     * schedule_locked()'s comment on thread vs. process completion. Every
     * process created today (process_create()/process_create_kthread())
     * starts with exactly one thread; thread_create() is what lets this
     * exceed 1. */
    int live_thread_count;
    struct thread *thread_list;   /* this process's threads, linked via thread::proc_thread_next */

    /* Meaningful ONLY once live_thread_count == 0 (the process has fully
     * exited): a mirror of the completing thread's OWN running_cpu at that
     * exact moment, kept in sync (cleared to -1) at the same instant
     * schedule_locked() actually switches that core away from it. Exists
     * because the hand-off onto a live parent's zombie_head (schedule_
     * locked()'s comment) happens BEFORE that switch-away is confirmed --
     * so a parent's process_wait() can observe this process in its zombie
     * list while the completing thread is still, for a brief window,
     * genuinely executing on another core. process_wait() retries
     * (unlock, pause, loop) while this is >= 0 rather than reaping an
     * address space a core might still be running on -- the exact same
     * belt-and-suspenders check the pre-split single-struct design used
     * on the PCB's own running_cpu field, preserved here at the process
     * level since running_cpu itself moved to struct thread. */
    int running_cpu;

    /* Per-process fd table (docs/architecture/process-and-scheduling.md
     * §6.2 -- unblocks the spawn() file-actions model docs/ARCHITECTURE.md
     * §3.2 commits to). fd.c's fd_table() operates on THIS array whenever
     * this process is current_process, instead of a single shared table. */
    struct fd_entry fds[FD_MAX_OPEN];

    /* Per-process handle table translating a ring-3-visible handle (what
     * sys_spawn returns, what sys_wait/sys_kill take) to the real pid --
     * see PROC_HANDLE_MAX's comment above. */
    struct proc_handle handles[PROC_HANDLE_MAX];

    /* Typed object-handle table for structural shared resources -- surfaces
     * (kernel/gfx/surface.h). Distinct from handles[] above (which is a
     * pid mapping): each entry points at a refcounted kernel object. */
    struct obj_handle obj_handles[OBJ_HANDLE_MAX];

    /* Bump allocator for this process's shared-memory VA window (slot 5,
     * USER_SHARED_VA_BASE): each surface mapping consumes a run here. VA
     * space is not reclaimed, same simplification as the kernel-stack and
     * MMIO allocators. */
    uint64_t shared_next_va;

    /* current break pointer for this process's heap (user-mode threads only) 
     * grows/shrinks as the process allocates/frees heap memory. via sys_brk(). */
    uint64_t heap_brk;  

    /* The highest virtual address mapped in the process's heap region (user-mode threads only) 
     * used to track the top of the heap and ensure that the process does not exceed its allocated heap space. */
    uint64_t heap_mapped_top;

};


/**
 * @brief Initialize the process management subsystem.
 *
 * This function sets up the necessary data structures and prepares the system
 * for process creation and scheduling.
 */
void process_init(void);

/**
 * @brief Change the end of the data segment (heap) for a process.
 *
 * @param proc The process whose heap is to be adjusted.
 * @param increment The amount by which to change the heap size.
 * @return int64_t The previous end of the heap on success, or a negative -EMBK_* code on failure.
 */
int64_t process_sbrk(struct process *proc, int64_t increment);

/**
 * @brief Create a new process.
 *
 * @param path The path to the executable for the new process.
 * @param argv argv[0..argc). Copied into the child's own stack before this
 *             returns, so the caller's storage need not outlive the call.
 * @param argc Number of entries in argv (NOT counting a NULL terminator --
 *             process_create() appends that itself; see SPAWN_ARGV_MAX's
 *             comment in spawn.h for the "+1 for the terminator" budget).
 * @return int The PID of the newly created process, or a negative -EMBK_*
 *             code on failure.
 */
int process_create(const char *path, char *const argv[], int argc,
                                 const struct spawn_file_action *actions, int n_count);

/**
 * @brief Switch to the next ready thread in the scheduler.
 */
void schedule(void);

/**
 * @brief Cooperative: Yield the CPU to allow other threads to run.
 *
 * This function is intended to be called by a running thread to voluntarily yield
 * control of the CPU, allowing the scheduler to select another ready thread to run.
 */
void sys_yield(void);

/**
 * @brief Reclaim a zombie process's resources (address space, PCB slot) and
 * return the slot to free (pid = 0). Only meaningful once the process has
 * NO live threads left -- see docs/architecture/process-and-scheduling.md's
 * Phase 4 thread/process split.
 *
 * Safe to call only once the process is no longer scheduled anywhere — the
 * scheduler itself defers reaping the thread/process it just switched away
 * from until the following schedule() call, once it's certain nothing is
 * still executing on that thread's kernel stack. Does nothing (logs and
 * returns) if `pid` isn't found or still has live threads.
 */
void process_reap(uint32_t pid);

/**
 * @brief Look up a process by pid without reaping or otherwise touching it.
 * Returns NULL if no such pid exists (never existed, or already reaped).
 */
struct process *process_find(uint32_t pid);

/**
 * @brief Block the given thread on a wait queue.
 *
 * Transitions RUNNING/READY -> BLOCKED and links it onto `wq`. `t` is
 * typically `current_thread` (blocking yourself), but the API takes an
 * explicit pointer since nothing else about it is self-referential.
 * Does not itself call schedule() — the caller must do that afterward for
 * a self-block to actually give up the CPU.
 */
void wait_queue_block(struct wait_queue *wq, struct thread *t);

/** @brief Wake the first thread waiting on `wq` (BLOCKED -> READY). No-op on an empty queue. */
void wait_queue_wake_one(struct wait_queue *wq);

/** @brief Wake every thread waiting on `wq` (BLOCKED -> READY). */
void wait_queue_wake_all(struct wait_queue *wq);

/* Blocking primitives for IPC and other kernel subsystems outside process.c.
 * sched_lock/unlock take/release the global scheduler lock (which the
 * wait_queue_* calls must be serialized under); sched_block_current_locked
 * blocks the caller on `wq` and switches away, returning with the lock
 * released once woken. See the block-until-woken usage note in process.c. */
void sched_lock(void);
void sched_unlock(void);
void sched_block_current_locked(struct wait_queue *wq);

/**
 * @brief Unconditionally terminate a process (every one of its threads),
 * regardless of current state (RUNNING, READY, or BLOCKED) and without its
 * cooperation.
 *
 * This is the "uncatchable kill" docs/ARCHITECTURE.md §3.3 requires day one:
 * a process that never cooperates with any cooperative signaling mechanism
 * must still be stoppable. Any thread blocked on a wait queue is first
 * unlinked from it (so a later wake can't walk into a dead TCB). A thread
 * that's the caller's own current_thread reschedules away from it; a
 * thread running on a different core is marked ZOMBIE only, and that
 * core's own next tick does the hand-off/reap (no IPI needed); a thread
 * that isn't running anywhere is reaped immediately.
 */
void process_kill(uint32_t pid);

/**
 * @brief Mark the current thread a zombie and hand off to the scheduler.
 * Shared by sys_exit (cpu/syscall.c) and the in-kernel selftests below,
 * which need the identical mark-zombie-then-reschedule path without going
 * through a syscall. Never returns.
 */
__attribute__((noreturn)) void process_exit_self(int code);

/**
 * @brief Create an in-kernel "thread": ring 0, sharing the kernel's own
 * address space, scheduled by the exact same round-robin/preemption/kill/
 * reap machinery as a real ring-3 process. `entry` should eventually call
 * process_exit_self() rather than returning.
 *
 * Convenience wrapper around thread_create(): allocates a FRESH process
 * (its own pid, sharing the kernel's PML4) with exactly this one thread.
 * `parent` is set BEFORE the kthread is marked schedulable (so there is no
 * window for it to run to completion parentless before the caller can
 * link it to anyone). Pass NULL for the usual fire-and-forget kthread
 * (auto-reaped on exit, never collected via process_wait()); pass a real
 * process only if that process intends to process_wait() on it.
 *
 * Exists so the scheduler selftests below don't need a real ELF file on
 * disk. Returns NULL if the process/thread table is full or the kernel
 * stack allocation fails.
 */
struct thread *process_create_kthread(void (*entry)(void), struct process *parent);

/**
 * @brief Create an ADDITIONAL in-kernel thread under an EXISTING process,
 * sharing its address space (pml4_phys) rather than the kernel's -- the
 * real multi-thread primitive (docs/architecture/process-and-scheduling.md
 * Phase 4). `entry` should eventually call process_exit_self() rather than
 * returning, same as process_create_kthread()'s.
 *
 * Ring 0 only -- for a ring-3-visible equivalent, see thread_create_user()
 * below (Phase 5). This kthread version stays auto-reaped/fire-and-forget
 * (`joinable == false`); nothing in this kernel joins a kthread today.
 * Returns NULL if the thread table is full or the kernel stack allocation
 * fails.
 */
struct thread *thread_create(struct process *proc, void (*entry)(void));

/**
 * @name Ring-3 threads (Phase 5, docs/architecture/process-and-scheduling.md)
 * The userspace-visible multi-thread primitive: an ADDITIONAL thread under
 * an EXISTING process, entering ring 3 directly (via process_trampoline,
 * the same fabricated-context mechanism process_create()'s own first
 * thread uses) with its own dedicated user stack inside that process's
 * address space -- see thread_create_user()'s definition (process.c) for
 * exactly where that stack lives and why it's never explicitly freed
 * per-thread (the whole address space, and everything mapped in it,
 * including every extra thread's stack, is reclaimed together when the
 * PROCESS itself is finally reaped -- a deliberate simplification, see
 * that comment).
 * @{
 */

/**
 * @brief Create a ring-3 thread under `proc`, entering at `entry_point`
 * with `arg` in RDI (mirroring pthread_create()'s `void *arg`, even though
 * there's no pthread of this kernel's own). Returns a small non-negative
 * TID -- the thread's own table slot, stable and unique while it lives,
 * meaningful only to OTHER threads of the SAME process (there is no
 * cross-process thread naming, unlike pids, which is why this returns a
 * raw integer rather than going through the capability-handle table
 * process.h's PROC_HANDLE_MAX section describes: a thread can only ever
 * name a sibling of its own process, so there's no confused-deputy gap to
 * close the way there is for one process naming another) -- or a negative
 * error code (-EMBK_EAGAIN if the thread table is full, -EMBK_ENOMEM on
 * allocation failure).
 *
 * The new thread is JOINABLE (see struct thread::joinable): it must
 * eventually be collected via thread_join(), or its slot is held forever.
 */
int thread_create_user(struct process *proc, uint64_t entry_point, uint64_t arg);

/**
 * @brief Block the caller until thread `tid` of `proc` (a joinable thread,
 * e.g. from thread_create_user()) exits, then reap it and return its own
 * exit code (struct thread::exit_code, NOT the process's). Returns
 * -EMBK_EINVAL if `tid` doesn't currently name a live-or-zombie joinable
 * thread of `proc` (unknown, wrong process, a non-joinable kthread, or
 * already joined by someone else -- a tid is single-use, exactly like a
 * process exit code is only ever collected once).
 */
int thread_join(struct process *proc, int tid);

/**
 * @brief Mark the CALLING thread a zombie and hand off to the scheduler --
 * the thread-level analog of process_exit_self(): ends only THIS thread,
 * not necessarily the whole process. If it happens to be the process's
 * last live thread, this also completes the process exactly as
 * process_exit_self() would (a parent's process_wait() sees a normal exit
 * with this same code) -- see the definition's comment for why that needs
 * no special-casing here. Never returns.
 */
__attribute__((noreturn)) void thread_exit_self(int code);
/** @} */

/* One dedicated hlt-loop idle kthread, pinned to cpu_table[cpu_index], at
 * PRIORITY_BACKGROUND. Called once per core from main.c after the process
 * subsystem is fully up -- see the definition's comment for why these are
 * a liveness requirement under SMP, not an optimization. */
struct thread *process_create_idle_for_cpu(uint32_t cpu_index);

/**
 * @name Scheduler selftests
 * docs/architecture/process-and-scheduling.md §12. Each returns 0 on
 * success, -1 on failure. Each temporarily takes over `current_thread`/the
 * round-robin scheduler for its duration and restores it to NULL before
 * returning — do not call while a real process is running.
 * @{
 */
int process_test_roundrobin(void);
int process_test_fpu(void);
int process_test_kill(void);
int process_test_reap(void);
int process_test_stackguard(void);
/** Exercises process_wait()'s two zombie hand-off paths (normal exit, uncatchable kill). */
int process_test_wait(void);
/** Exercises priority bands + aging (docs/architecture/process-and-scheduling.md §4.2 Phase C). */
int process_test_priority(void);
/** Proves genuinely concurrent cross-core dispatch: >= 2 distinct cores must run the kthreads. */
int process_test_smp_sched(void);
/** Exercises process_kill()'s running_elsewhere branch (kill a kthread live on another core). */
int process_test_smp_kill(void);
/** Proves multiple threads under ONE process genuinely share its address space (write via one, read via another) and each first-executes with its own core/stack. */
int process_test_thread_smp(void);
/** Proves a process's address space survives until its LAST thread exits, not its first. */
int process_test_thread_exit(void);
/** @} */

/**
 * @brief Block the caller until a SPECIFIC child (by pid) exits, then reap
 * it and return its exit code.
 *
 * `pid` must be a live or already-exited-but-unreaped child of
 * `current_process` (i.e. `process_find(pid)->parent == current_process`,
 * or it's already sitting in `current_process->zombie_head`) -- returns
 * -EMBK_ECHILD otherwise (not our child, or already reaped by an earlier
 * wait() call). If the child hasn't exited yet, blocks on
 * `current_process->child_wait` and re-checks every time ANY child of ours
 * exits, since child_wait has no way to say which child woke it.
 */
int process_wait(uint32_t pid);

/**
 * @brief Turn the CALLING execution context into a real, schedulable
 * `current_thread` (owned by a fresh process sharing the kernel's own
 * address space) -- the same trick the scheduler selftests use internally
 * (see process_create_kthread()'s comment), exposed here so the kernel's
 * interactive shell (main.c) can become a normal round-robin participant.
 * Unlike the selftest version, this is meant to be permanent for the life
 * of the kernel (the shell never "exits"), so it is not paired with a
 * restore function.
 */
struct thread *process_adopt_current(void);

/**
 * @brief Set a process's priority band, clamped to
 * [PRIORITY_REALTIME, PRIORITY_BACKGROUND]. Returns 0 on success,
 * -EMBK_ECHILD if `pid` doesn't exist.
 */
int process_set_priority(uint32_t pid, uint8_t priority);

/** Snapshot of one process, for `ps`-style listings (process_list() below) -- deliberately NOT a `struct process *`, so callers outside process.c never get a live pointer into the process table. */
struct process_info {
    uint32_t pid;
    uint32_t parent_pid;   /* 0 if none */
    enum process_state state;
    uint8_t priority;
    int exit_code;         /* meaningful only when state == PROCESS_ZOMBIE */
    bool is_kthread;        /* shares the kernel's own PML4 rather than owning one */
};

/**
 * @brief Fill `out` (capacity `max`) with a snapshot of every non-UNUSED
 * process, for `ps`. Returns the number of entries written (<= max).
 */
int process_list(struct process_info *out, int max);

/* Self-locking liveness probe: 1 if `pid` exists with a live thread, else 0.
 * A snapshot (inherently racy); used by sys_proc_alive so the home launcher
 * can avoid double-spawning an app that is still running. */
int process_alive(uint32_t pid);

/**
 * @name Ring-3 process handle table (docs/ARCHITECTURE.md §3.4/§3.5)
 * A handle is a small integer meaningful only to the process that holds
 * it; sys_spawn (cpu/syscall.c) allocates one in the CALLER's table
 * pointing at the new child's pid, sys_wait/sys_kill resolve one back to
 * a pid before acting on it. A process can only ever name a pid it was
 * actually handed a handle to -- closes the confused-deputy gap a raw pid
 * argument would otherwise leave open at the ring-3 boundary.
 * @{
 */
/** Allocate a handle in `owner`'s table pointing at `pid`. Returns the handle (>= 0), or -EMBK_EMFILE if the table is full. */
int process_handle_alloc(struct process *owner, uint32_t pid);
/** Resolve `handle` in `owner`'s table to a pid. Returns 0 and writes *out_pid on success, -EMBK_EINVAL if `handle` is out of range or unused. */
int process_handle_resolve(struct process *owner, int handle, uint32_t *out_pid);
/** Free `handle` in `owner`'s table (idempotent -- freeing an already-free or out-of-range handle is a silent no-op). */
void process_handle_free(struct process *owner, int handle);
/** Reclaim handle slots naming already-exited children (reaping those zombies);
 *  leaves live children alone. Returns the count reclaimed. `owner` must be
 *  current_process. Lets a full handle table self-heal instead of forcing a
 *  spawn to kill its new child. */
int process_handle_reap_dead(struct process *owner);
/** @} */

/* current_thread is the REAL per-core field (kernel/cpu/percpu.h's struct
 * cpu_data) -- the assignable lvalue every internal scheduler function
 * (process.c) reads and writes: `current_thread = next;`,
 * `current_thread->state`, etc. current_process is a DERIVED, read-only
 * convenience (`current_thread->proc`) for the external callers that only
 * ever want the owning process's fields -- cpu/syscall.c's
 * `current_process->pid`, cpu/usercopy.c's `current_process->pml4_phys`,
 * fs/fd.c's `current_process->fds` all keep working completely unchanged
 * (Phase 4's whole design goal: the split is invisible outside process.c).
 *
 * NOT assignable: `current_process = x` would silently expand to
 * `current_thread->proc = x`, reassigning the CURRENT thread's owner
 * in place -- never what any caller actually means. Every place that
 * used to write `current_process = x` now writes `current_thread = x`
 * (a real thread pointer) instead; process.c is the only file that does
 * this, so this isn't a compatibility concern for anything outside it.
 *
 * current_thread can be NULL (no schedulable context yet, e.g. very early
 * boot); current_process can't be safely evaluated in that state (it would
 * dereference a NULL current_thread) -- every external NULL-check
 * (cpu/usercopy.c's access_ok(), fs/fd.c's fd_table()) checks
 * current_thread, not current_process, for exactly this reason. `this_cpu()`
 * itself is declared in percpu.h, included above. */
#define current_thread (this_cpu()->current_thread)
#define current_process (current_thread->proc)

#endif /* __PROCESS_H__ */