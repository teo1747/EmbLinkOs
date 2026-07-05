#ifndef __PROCESS_H__
#define __PROCESS_H__

#include <stdint.h>
#include "../include/types.h"
#include "../cpu/kcontext.h"
#include "../fs/fd.h"

#define MAX_PROCESSES 16
#define KSTACK_SIZE (MAX_PROCESSES * 1024)  // 16 KiB per process kernel stack

enum process_state {
    PROCESS_UNUSED = 0,      /**< Process slot is unused */
    PROCESS_READY,            /**< Process is ready to run */
    PROCESS_RUNNING,          /**< Process is currently running */
    PROCESS_BLOCKED,          /**< Process is blocked/waiting */
    PROCESS_ZOMBIE            /**< Process has terminated but not yet cleaned up */
};

/* Intrusive wait queue: blocked processes are linked through their own
 * process::wait_next field (see docs/architecture/process-and-scheduling.md
 * §7.3), so no separate allocation is needed to queue a process. */
struct wait_queue {
    struct process *head;
};


struct process {
    uint32_t pid;              /**< Process ID */
    uint64_t pml4_phys;        /**< Physical address of the process's PML4 (page table) */
    struct kcontext ctx;       /**< Saved kernel context for this process */
    uint64_t kstack_top;       /**< Virtual address of the top of the kernel stack for this process */
    uint64_t entry_point;      /**< Entry point for the process (user mode) */
    uint64_t user_rsp;         /**< User mode stack pointer */
    enum process_state state;  /**< Current state of the process */

    /* Wait-queue membership. wait_queue is NULL unless state == BLOCKED;
     * process_kill() uses it to unlink a killed process from whatever queue
     * it's blocked on, so a later wake can't walk into a dead PCB. */
    struct process *wait_next;
    struct wait_queue *wait_queue;

    int exit_code;             /**< Exit code if the process has terminated */

    /* Per-process fd table (docs/architecture/process-and-scheduling.md
     * §6.2 -- unblocks the spawn() file-actions model docs/ARCHITECTURE.md
     * §3.2 commits to). fd.c's fd_table() operates on THIS array whenever
     * this process is current_process, instead of a single shared table. */
    struct fd_entry fds[FD_MAX_OPEN];
};


/**
 * @brief Initialize the process management subsystem.
 *
 * This function sets up the necessary data structures and prepares the system
 * for process creation and scheduling.
 */
void process_init(void);

/**
 * @brief Create a new process.
 *
 * @param path The path to the executable for the new process.
 * @return int The PID of the newly created process, or -1 on failure.
 */
int process_create(const char *path);

/**
 * @brief Switch to the next ready process in the scheduler.
 */
void schedule(void);

/**
 * @brief Cooperative: Yield the CPU to allow other processes to run.
 *
 * This function is intended to be called by a running process to voluntarily yield
 * control of the CPU, allowing the scheduler to select another ready process to run.
 */
void sys_yield(void);

/**
 * @brief Start the first process.
 *
 * This function selects the first READY process, sets up its address space and kernel stack,
 * marks it as RUNNING, and restores its context to start execution.
 */
void process_start_first(void);

/**
 * @brief Reclaim a zombie process's resources (address space, kernel stack,
 * PCB slot) and return the slot to PROCESS_UNUSED.
 *
 * Safe to call only once the process is no longer scheduled anywhere — the
 * scheduler itself defers reaping the process it just switched away from
 * until the following schedule() call, once it's certain nothing is still
 * executing on that process's kernel stack. Does nothing (logs and returns)
 * if `pid` isn't found or isn't a zombie.
 */
void process_reap(uint32_t pid);

/**
 * @brief Look up a process by pid without reaping or otherwise touching it.
 * Returns NULL if no such pid exists (never existed, or already reaped).
 */
struct process *process_find(uint32_t pid);

/**
 * @brief Block the given process on a wait queue.
 *
 * Transitions RUNNING/READY -> BLOCKED and links it onto `wq`. `p` is
 * typically `current_process` (blocking yourself), but the API takes an
 * explicit pointer since nothing else about it is self-referential.
 * Does not itself call schedule() — the caller must do that afterward for
 * a self-block to actually give up the CPU.
 */
void wait_queue_block(struct wait_queue *wq, struct process *p);

/** @brief Wake the first process waiting on `wq` (BLOCKED -> READY). No-op on an empty queue. */
void wait_queue_wake_one(struct wait_queue *wq);

/** @brief Wake every process waiting on `wq` (BLOCKED -> READY). */
void wait_queue_wake_all(struct wait_queue *wq);

/**
 * @brief Unconditionally terminate a process, regardless of its current
 * state (RUNNING, READY, or BLOCKED) and without its cooperation.
 *
 * This is the "uncatchable kill" docs/ARCHITECTURE.md §3.3 requires day one:
 * a process that never cooperates with any cooperative signaling mechanism
 * must still be stoppable. If the target is blocked on a wait queue, it is
 * first unlinked from it (so a later wake can't walk into a dead PCB).
 * Killing the currently-running process reschedules away from it; killing
 * any other process reaps it immediately (safe: on this single core, only
 * `current_process` is ever actually executing).
 */
void process_kill(uint32_t pid);

/**
 * @brief Mark the current process a zombie and hand off to the scheduler.
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
 * Exists so the scheduler selftests below don't need a real ELF file on
 * disk. Returns NULL if the process table is full or the kernel stack
 * allocation fails.
 */
struct process *process_create_kthread(void (*entry)(void));

/**
 * @name Scheduler selftests
 * docs/architecture/process-and-scheduling.md §12. Each returns 0 on
 * success, -1 on failure. Each temporarily takes over `current_process`/the
 * round-robin scheduler for its duration and restores it to NULL before
 * returning — do not call while a real process is running.
 * @{
 */
int process_test_roundrobin(void);
int process_test_kill(void);
int process_test_reap(void);
int process_test_stackguard(void);
/** @} */

extern struct process *current_process;  /**< Pointer to the currently running process */

#endif /* __PROCESS_H__ */