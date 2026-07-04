#include "process.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../include/kmalloc.h"
#include "../include/kprintf.h"
#include "../include/errno.h"
#include "../drivers/serial.h"
#include "../cpu/elf.h"
#include <stdint.h>

#define USER_STACK_TOP 0x0000700000001000ULL  /* TOP of the user stack  page*/
#define USER_STACK_VA  0x0000700000000000ULL  /* base of the user stack page */


static struct process proc_table[MAX_PROCESSES];
struct process *current_process = NULL;
static uint32_t next_pid = 1;  // Start PIDs from 1, 0 is reserved for the kernel


/* Forward declarations: the trampoline is the fabricated cte.rip - where
 * a brand-new process "resumes" the first time the scheduler switches to it. */
static void process_trampoline(void);  

/* First free slot. Same static table + state-marker pattern as the mount
 * fd, and open-ref tables. */
static struct process *proc_alloc(void) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (proc_table[i].state == PROCESS_UNUSED) {
            proc_table[i].pid = next_pid++;
            proc_table[i].state = PROCESS_READY;
            return &proc_table[i];
        }
    }
    return NULL;  // No free slots
}

/* A per-process kernel stack. it must be reachable in EVERY address space.
 * because an interrupt or syscall can land while any process's CR3 is active. */
static uint64_t alloc_kernel_stack(void) {
    uint8_t *base = kmalloc(KSTACK_SIZE);
    if (!base) return 0;
    return (uint64_t)(uintptr_t)base + KSTACK_SIZE;  // Return the top of the stack (stack grows down)
}

/* Create a new process from an ELF executable */
int process_create(const char *path){
    struct process *proc = proc_alloc();
    if (!proc) {
        return -EMBK_ENOMEM;  // No free process slots
    }

    // 1. Its own address space (PML4) with the kernel half mapped
    uint64_t pml4 = vmm_create_address_space();
    if (!pml4) {
        return -EMBK_ENOMEM;  // Failed to create address space
    }
    
    /* 2. Load the ELF into that addresse space (via the direct map-cr3 stays kernel
     * elf_load maps into pml4 and copies through P2V). stash the entry point for the trampoline jump to*/

    uint64_t entry_point = 0;
    //int rc = elf_load_from_file(path, pml4, &entry_point);
    //if (rc != EMBK_OK) {
    //   vmm_destroy_address_space(pml4);
    //    return rc;  // ELF load failed
    //}

    // 3. Allocate a user stack page and map it into the process's address space
    uint64_t stack_phys = pmm_alloc_page();
    if (!stack_phys) {
        vmm_destroy_address_space(pml4);
        return -EMBK_ENOMEM;  // Failed to allocate user stack
    }

    vmm_map_in(pml4, USER_STACK_VA, stack_phys, VMM_NX | VMM_WRITABLE | VMM_USER);

    // 4. Allocate a kernel stack for this process
    uint64_t kstack_top = alloc_kernel_stack();
    if (!kstack_top) {
        pmm_free_page(stack_phys);
        vmm_destroy_address_space(pml4);
        return -EMBK_EIO;  // Failed to allocate kernel stack
    }

    // 5. Initialize the process structure
    proc->pid = next_pid++;
    proc->pml4_phys = pml4;
    proc->kstack_top = kstack_top;
    proc->entry_point = entry_point;
    proc->user_rsp = USER_STACK_TOP;
    proc->exit_code = 0;

    /* 6. FABRICATE the kernel context so the first schedule()-in lands the
     *    process at the trampoline, on its own kernel stack, interrupts on.
          This is the "make cxt look like a freshly interrupted context"*/

    proc->ctx.rbx = proc->ctx.rbp = 0;
    proc->ctx.r12 = proc->ctx.r13 = proc->ctx.r14 = proc->ctx.r15 = 0;
    proc->ctx.rip = (uint64_t)(uintptr_t)process_trampoline;  // trampoline will jump to entry_point
    proc->ctx.rsp = kstack_top;  // Start at the top of the kernel stack
    proc->ctx.rflags = 0x202;  // Interrupts enabled (IF=1)

    proc->state = PROCESS_READY;
    return (int)proc->pid;
}

/* The fabricated ctx.rip. Runs (on the new process's kernel stack) the first time
 * the scheduler switches to this process. scheduler() has ALREADY switched CR3 to
 * this process's address space and set TSS.rsp0 before restoring, so here CR3 and
 * the kernel stack are already correct. the trampoline sets up the user stack and jumps to the entry point. 
 */
static void process_trampoline(void) {
    uint64_t entry = current_process->entry_point;
    uint64_t user_rsp = current_process->user_rsp;

    /* Set up the user stack and jump to the entry point */
    __asm__ volatile(
        "pushq %0\n"            // ss = user data | 3
        "pushq %1\n"            // rsp = user stack top
        "pushq $0x202\n"        // rflags = IF=1
        "pushq $2\n"            // cs = user code | 3
        "pushq %3\n"            // rip = entry point
        "iretq\n"               // return to user mode
        :
        : "r"((uint64_t)(0x18 | 3)), "r"(user_rsp),
          "r"((uint64_t)(0x20 | 3)), "r"(entry)
        : "memory"
    );
    __builtin_unreachable();  // Should never return
}

void process_init(void) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        proc_table[i].state = PROCESS_UNUSED;
    }
    current_process = NULL;
}



