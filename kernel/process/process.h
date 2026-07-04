#ifndef __PROCESS_H__
#define __PROCESS_H__

#include <stdint.h>
#include "../cpu/kcontext.h"

#define MAX_PROCESSES 16
#define KSTACK_SIZE (MAX_PROCESSES * 1024)  // 16 KiB per process kernel stack

enum process_state {
    PROCESS_UNUSED = 0,      /**< Process slot is unused */
    PROCESS_READY,            /**< Process is ready to run */
    PROCESS_RUNNING,          /**< Process is currently running */
    PROCESS_BLOCKED,          /**< Process is blocked/waiting */
    PROCESS_ZOMBIE            /**< Process has terminated but not yet cleaned up */
};


struct process {
    uint32_t pid;              /**< Process ID */
    uint64_t pml4_phys;        /**< Physical address of the process's PML4 (page table) */
    struct kcontext ctx;       /**< Saved kernel context for this process */
    uint16_t kstack_top;       /**< Offset to the top of the kernel stack for this process */
    uint64_t entry_point;      /**< Entry point for the process (user mode) */
    uint64_t user_rsp;         /**< User mode stack pointer */
    enum process_state state;  /**< Current state of the process */
   
    int exit_code;             /**< Exit code if the process has terminated */
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

extern struct process *current_process;  /**< Pointer to the currently running process */

#endif /* __PROCESS_H__ */