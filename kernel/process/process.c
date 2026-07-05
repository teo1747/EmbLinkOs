#include "process.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../include/kprintf.h"
#include "../include/errno.h"
#include "../include/kstring.h"
#include "../cpu/elf.h"
#include "../cpu/gdt.h"
#include "../cpu/kcontext.h"
#include "../cpu/lapic.h"
#include <stdint.h>

#define USER_STACK_TOP 0x0000700000001000ULL  /* TOP of the user stack  page*/
#define USER_STACK_VA  0x0000700000000000ULL  /* base of the user stack page */


static struct process proc_table[MAX_PROCESSES];
struct process *current_process = NULL;
static uint32_t next_pid = 1;  // Start PIDs from 1, 0 is reserved for the kernel

// A zombie the scheduler just switched away from. We can't reclaim its
// kernel stack/address space immediately — we're still executing on that
// very stack for the remainder of this schedule() call, up to and including
// the kernel_ctx_switch that leaves it. It's reclaimed the *next* time
// schedule() runs (top of the function, before anything else touches the
// tables), by which point execution is guaranteed to be on a different
// process's stack. One slot is enough: every exit calls schedule() itself,
// so this never has more than one zombie queued behind it in practice.
static struct process *g_pending_reap = NULL;


/* Forward declarations: the trampoline is the fabricated cte.rip - where
 * a brand-new process "resumes" the first time the scheduler switches to it. */
static void process_trampoline(void);  

/* First free slot. Same static table + state-marker pattern as the mount
 * fd, and open-ref tables. */
/* Marks the slot PROCESS_BLOCKED, not PROCESS_READY: the caller
 * (process_create()/process_create_kthread()) still has to set up the
 * address space, kernel stack, and fabricated ctx before this PCB is safe
 * to schedule. PROCESS_BLOCKED is excluded from schedule()'s candidate
 * search (same as any other blocked process), so it's a safe placeholder
 * even though this slot isn't actually on any wait_queue (process_kill()
 * already guards its own wait_queue-removal on wait_queue != NULL, so a
 * BLOCKED-but-queueless slot doesn't confuse it either). The caller flips
 * it to PROCESS_READY as its own last step, once genuinely safe to run.
 *
 * This was originally state = PROCESS_READY here, immediately -- harmless
 * at boot (current_process was NULL for the whole risky window, so
 * schedule() no-op'd regardless of what raced), but a real bug once a
 * second process + timer preemption coexist: the timer could fire mid-
 * process_create() (e.g. during the ELF load's disk I/O -- slow enough to
 * make the window real) and schedule() would pick this half-built PCB,
 * switching to a zeroed/garbage pml4_phys and ctx. */
static struct process *proc_alloc(void) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (proc_table[i].state == PROCESS_UNUSED) {
            proc_table[i].pid = next_pid++;
            proc_table[i].state = PROCESS_BLOCKED;
            return &proc_table[i];
        }
    }
    return NULL;  // No free slots
}

/* A per-process kernel stack. it must be reachable in EVERY address space.
 * because an interrupt or syscall can land while any process's CR3 is active.
 * Page-mapped with an unmapped guard page below it (vmm_alloc_kernel_stack) —
 * a plain kmalloc'd stack has no guard page, so an overflow silently
 * corrupts whatever heap allocation happens to sit next to it instead of
 * faulting at the overflow site. */
static uint64_t alloc_kernel_stack(void) {
    return vmm_alloc_kernel_stack(KSTACK_SIZE);
}

/* Reclaim everything a PCB owns and return the slot to PROCESS_UNUSED.
 * Only called on a process that is guaranteed not to be executing anywhere
 * (see g_pending_reap's comment) — never on `current_process`. */
static void process_reap_slot(struct process *proc) {
    if (proc->state != PROCESS_ZOMBIE) {
        kprintf("process_reap: pid %u is not a zombie (state=%d), refusing\n",
                (unsigned int)proc->pid, (int)proc->state);
        return;
    }

    vmm_destroy_address_space(proc->pml4_phys);
    vmm_free_kernel_stack(proc->kstack_top, KSTACK_SIZE);

    uint32_t reaped_pid = proc->pid;
    memset(proc, 0, sizeof(*proc));
    proc->state = PROCESS_UNUSED;

    kprintf("process_reap: pid %u reclaimed\n", (unsigned int)reaped_pid);
}

void process_reap(uint32_t pid) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (proc_table[i].pid == pid && proc_table[i].state != PROCESS_UNUSED) {
            process_reap_slot(&proc_table[i]);
            return;
        }
    }
    kprintf("process_reap: pid %u not found\n", (unsigned int)pid);
}

struct process *process_find(uint32_t pid) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (proc_table[i].pid == pid && proc_table[i].state != PROCESS_UNUSED) {
            return &proc_table[i];
        }
    }
    return NULL;
}

/* --------------------------------------------------------------------
 * Wait queues (docs/architecture/process-and-scheduling.md §7.3)
 * -------------------------------------------------------------------- */

void wait_queue_block(struct wait_queue *wq, struct process *p) {
    p->state = PROCESS_BLOCKED;
    p->wait_queue = wq;
    p->wait_next = wq->head;
    wq->head = p;
}

/* Unlink `p` from `wq`'s intrusive list without changing its state — used
 * by both wait_queue_wake_one() (which then sets READY) and process_kill()
 * (which then sets ZOMBIE). Keeping removal separate from the state change
 * means a killed-while-blocked process can't leave a dangling entry that a
 * later wait_queue_wake_*() call would walk into. */
static void wait_queue_remove(struct wait_queue *wq, struct process *p) {
    struct process **link = &wq->head;
    while (*link) {
        if (*link == p) {
            *link = p->wait_next;
            p->wait_next = NULL;
            p->wait_queue = NULL;
            return;
        }
        link = &(*link)->wait_next;
    }
}

void wait_queue_wake_one(struct wait_queue *wq) {
    if (!wq->head) {
        return;
    }
    struct process *p = wq->head;
    wait_queue_remove(wq, p);
    p->state = PROCESS_READY;
}

void wait_queue_wake_all(struct wait_queue *wq) {
    while (wq->head) {
        wait_queue_wake_one(wq);
    }
}

/* --------------------------------------------------------------------
 * Uncatchable kill (docs/ARCHITECTURE.md §3.3 — ships with the scheduler,
 * not gated on the (much larger, not-yet-built) message-port IPC model).
 * -------------------------------------------------------------------- */

void process_kill(uint32_t pid) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        struct process *p = &proc_table[i];
        if (p->pid != pid || p->state == PROCESS_UNUSED) {
            continue;
        }
        if (p->state == PROCESS_ZOMBIE) {
            return;   // already dying; nothing left to force
        }

        bool was_current = (p == current_process);

        if (p->state == PROCESS_BLOCKED && p->wait_queue) {
            wait_queue_remove(p->wait_queue, p);
        }

        p->exit_code = -1;         // killed, not a normal exit
        p->state = PROCESS_ZOMBIE;

        if (was_current) {
            // Same deferred-reap dance sys_exit uses: we're still executing
            // on this process's kernel stack until schedule()'s eventual
            // kernel_ctx_switch actually happens (see g_pending_reap).
            schedule();
        } else {
            // Definitely not executing anywhere on this single core — safe
            // to reclaim its resources immediately rather than waiting for
            // a schedule() call that may never involve it as `prev`.
            process_reap_slot(p);
        }
        return;
    }
    kprintf("process_kill: pid %u not found\n", (unsigned int)pid);
}

/* Shared by sys_exit and the kthread selftests below: mark the current
 * process a zombie and hand off to the scheduler. If nothing else is
 * runnable, schedule() returns here and there's nothing left to do on this
 * core. Never returns. */
__attribute__((noreturn))
void process_exit_self(int code) {
    current_process->exit_code = code;
    current_process->state = PROCESS_ZOMBIE;
    schedule();

    for (;;) {
        __asm__ volatile("cli; hlt");
    }
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
        proc->state = PROCESS_UNUSED;   // undo proc_alloc()'s reservation
        return -EMBK_ENOMEM;  // Failed to create address space
    }

    /* 2. Load the ELF into that addresse space (via the direct map-cr3 stays kernel
     * elf_load maps into pml4 and copies through P2V). stash the entry point for the trampoline jump to*/

    uint64_t entry_point = 0;
    int rc = elf_load_from_file(path, pml4, &entry_point);
    if (rc != EMBK_OK) {
        vmm_destroy_address_space(pml4);
        proc->state = PROCESS_UNUSED;
        return rc;  // ELF load failed
    }

    // 3. Allocate a user stack page and map it into the process's address space
    uint64_t stack_phys = pmm_alloc_page();
    if (!stack_phys) {
        vmm_destroy_address_space(pml4);
        proc->state = PROCESS_UNUSED;
        return -EMBK_ENOMEM;  // Failed to allocate user stack
    }

    vmm_map_in(pml4, USER_STACK_VA, stack_phys, VMM_NX | VMM_WRITABLE | VMM_USER);

    // 4. Allocate a kernel stack for this process
    uint64_t kstack_top = alloc_kernel_stack();
    if (!kstack_top) {
        pmm_free_page(stack_phys);
        vmm_destroy_address_space(pml4);
        proc->state = PROCESS_UNUSED;
        return -EMBK_EIO;  // Failed to allocate kernel stack
    }

    // 5. Initialize the process structure
    // (proc->pid was already assigned by proc_alloc() above — reassigning it
    // here used to silently skip a PID every single process_create() call.)
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
        "pushq %2\n"            // cs = user code | 3
        "pushq %3\n"            // rip = entry point
        "iretq\n"               // return to user mode
        :
        : "r"((uint64_t)(0x18 | 3)), "r"(user_rsp),
          "r"((uint64_t)(0x20 | 3)), "r"(entry)
        : "memory"
    );
    __builtin_unreachable();  // Should never return
}

/* Round-robin scheduler: from the process after current, find the next READY (or the
 * current one if it's still runnable and nothing else is). Switch address space + 
 * kernel stack, then context-switch into it. */
void schedule(void) {
    if (!current_process) return;  // No process to schedule

    /* Reclaim the zombie (if any) we switched away from on the *previous*
     * schedule() call. Safe here specifically: getting back to the top of
     * schedule() at all means we've been switched back into a live process
     * since then, so nothing is executing on the zombie's kernel stack
     * anymore. See g_pending_reap's declaration for why this can't happen
     * any earlier than this. */
    if (g_pending_reap) {
        struct process *zombie = g_pending_reap;
        g_pending_reap = NULL;
        process_reap_slot(zombie);
    }

    /* find the next READY process */
    int start = 0;
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (&proc_table[i] == current_process) {
            start = i;
            break;
        }
    }
    struct process *next = NULL;
    for (int off = 1; off <= MAX_PROCESSES; off++) {
        struct process *candidate = &proc_table[(start + off) % MAX_PROCESSES];
        if (candidate->state == PROCESS_READY || candidate->state == PROCESS_RUNNING) {
            next = candidate;
            break;
        }
    }

    if (!next || next == current_process) {
        // No other process to switch to, or only the current one is runnable
        return;
    }

    struct process *prev = current_process;
    if (prev->state == PROCESS_RUNNING) {
        prev->state = PROCESS_READY;  // Mark the previous process as READY
    } else if (prev->state == PROCESS_ZOMBIE) {
        // Queue for reclamation at the top of the *next* schedule() call —
        // not now, we're still running on prev's kernel stack until the
        // kernel_ctx_switch below actually happens.
        g_pending_reap = prev;
    }
    next->state = PROCESS_RUNNING;  // Mark the next process as RUNNING
    current_process = next;

    /* Switch the incoming process's address space + kernel stack BEFORE the
     * context switch - uniform for resumed and brand-new processes (the trampoline
     * then needs neither). safe window: kernel half is shared, so flapping CR3 while
     * still on prev's kernel stack keeps executing fine. */
    vmm_switch_address_space(next->pml4_phys);
    tss_set_rsp0(next->kstack_top);

    kernel_ctx_switch(&prev->ctx, &current_process->ctx);
    
}

void sys_yield(void) {
    schedule();
}

/* Start the first process: no outgoing context switch/save, so this is a ONE-WAY
 * restore into the first process, not a context switch. Set up its address space
 * and kernel stack, marks it running, and restore its fabricated context, then jump to its entry point. */
void process_start_first(void) {
    struct process *first = NULL;
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (proc_table[i].state == PROCESS_READY) {
            first = &proc_table[i];
            break;
        }
    }
    if (!first) {
        kprintf("No READY process to start!\n");
        return;
    }

    first->state = PROCESS_RUNNING;
    current_process = first;
    
    /* same incoming-side setup schedule() does: CR3 + RSP0 before entering */
    vmm_switch_address_space(first->pml4_phys);
    tss_set_rsp0(first->kstack_top);

    /* One way restore of fabricated context -> jmps to process_trampoline 
     * 1 is the ret_val the kernel_ctx_restore forces non-zero anyway */
    kernel_ctx_restore(&first->ctx, 1);

    /* Should never return here */
    __builtin_unreachable();
}



void process_init(void) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        proc_table[i].state = PROCESS_UNUSED;
    }
    current_process = NULL;
}

/* ==========================================================================
 * In-kernel test "threads" + scheduler selftests
 * (docs/architecture/process-and-scheduling.md §12)
 *
 * A kthread is ring 0 and shares the kernel's own address space rather than
 * creating one — vmm_destroy_address_space() explicitly no-ops on
 * kernel_pml4_phys (see vmm.c), so reaping one never touches the shared
 * kernel tables. That's the only thing that makes it a "kthread" rather
 * than a real process: everything else (PCB, kernel stack + guard page,
 * scheduling, reaping, killing) goes through the exact same code a real
 * ring-3 process does.
 * ========================================================================== */

struct process *process_create_kthread(void (*entry)(void)) {
    struct process *proc = proc_alloc();
    if (!proc) {
        return NULL;
    }

    uint64_t kstack_top = alloc_kernel_stack();
    if (!kstack_top) {
        proc->state = PROCESS_UNUSED;   // undo proc_alloc()'s reservation
        return NULL;
    }

    proc->pml4_phys = vmm_get_kernel_pml4();
    proc->kstack_top = kstack_top;
    proc->entry_point = 0;    // unused: kthreads run in ring 0, no ring-3 trampoline
    proc->user_rsp = 0;       // unused
    proc->exit_code = 0;

    proc->ctx.rbx = proc->ctx.rbp = 0;
    proc->ctx.r12 = proc->ctx.r13 = proc->ctx.r14 = proc->ctx.r15 = 0;
    proc->ctx.rip = (uint64_t)(uintptr_t)entry;   // straight to `entry`, no trampoline needed
    proc->ctx.rsp = kstack_top;
    proc->ctx.rflags = 0x202;

    proc->state = PROCESS_READY;
    return proc;
}

/* Turn the CALLING context (whatever is executing this selftest right now)
 * into a real `current_process`, so the timer-driven scheduler has
 * something to round-robin the kthreads created below against —
 * schedule() is a no-op whenever current_process is NULL, no matter how
 * many kthreads sit READY in proc_table. `self->ctx` starts as whatever
 * proc_alloc() zeroed it to; that's fine, because it is only ever WRITTEN
 * (by the first schedule() that preempts us) before it is ever READ (by
 * the schedule() call that later resumes us) — this function's own stack
 * and call frame are never touched by kernel_ctx_switch, so returning
 * normally afterward, having been invisibly preempted and resumed any
 * number of times in between, is safe. */
static struct process *selftest_become_current(void) {
    struct process *self = proc_alloc();
    if (!self) {
        return NULL;
    }
    self->pml4_phys = vmm_get_kernel_pml4();
    self->state = PROCESS_RUNNING;
    current_process = self;
    return self;
}

/* Undo selftest_become_current(): `self` never owned a real kernel stack or
 * address space (it borrowed the caller's own stack and the kernel's own
 * PML4), so it's reset directly rather than through process_reap_slot()
 * (which would wrongly try to free page 0 / vmm_destroy the kernel's PML4). */
static void selftest_restore_current(struct process *self) {
    memset(self, 0, sizeof(*self));
    self->state = PROCESS_UNUSED;
    current_process = NULL;
}

static void selftest_wait_ticks(uint64_t ticks) {
    uint64_t start = lapic_timer_get_ticks();
    while (lapic_timer_get_ticks() < start + ticks) {
        __asm__ volatile("hlt");
    }
}

#define RR_KTHREAD_COUNT 3
static volatile uint64_t g_rr_counter[RR_KTHREAD_COUNT];
static volatile bool     g_rr_stop;

static void rr_kthread_entry_0(void) { while (!g_rr_stop) { g_rr_counter[0]++; } process_exit_self(0); }
static void rr_kthread_entry_1(void) { while (!g_rr_stop) { g_rr_counter[1]++; } process_exit_self(0); }
static void rr_kthread_entry_2(void) { while (!g_rr_stop) { g_rr_counter[2]++; } process_exit_self(0); }

/* Proves real preemption, not just "multiple processes can exist": three
 * kthreads spin incrementing their own counter with NO voluntary yield —
 * the ONLY thing that can give any of them CPU time is the timer IRQ's
 * unconditional schedule() call. If round-robin didn't work, at most one
 * counter would ever move (whichever kthread got picked first would spin
 * forever, since it never yields). */
int process_test_roundrobin(void) {
    struct process *self = selftest_become_current();
    if (!self) {
        return -1;
    }

    g_rr_counter[0] = g_rr_counter[1] = g_rr_counter[2] = 0;
    g_rr_stop = false;

    void (*entries[RR_KTHREAD_COUNT])(void) = {
        rr_kthread_entry_0, rr_kthread_entry_1, rr_kthread_entry_2
    };
    struct process *t[RR_KTHREAD_COUNT];
    bool ok = true;
    for (int i = 0; i < RR_KTHREAD_COUNT; i++) {
        t[i] = process_create_kthread(entries[i]);
        if (!t[i]) {
            ok = false;
        }
    }

    if (ok) {
        selftest_wait_ticks(20);   // ~200 ms at 100 Hz: many rounds for 4 "processes"
        ok = g_rr_counter[0] > 0 && g_rr_counter[1] > 0 && g_rr_counter[2] > 0;
        kprintf("process_test_roundrobin: counters = %u, %u, %u\n",
                (unsigned int)g_rr_counter[0], (unsigned int)g_rr_counter[1],
                (unsigned int)g_rr_counter[2]);
    }

    g_rr_stop = true;
    selftest_wait_ticks(8);   // let all three notice the flag, exit, and get reaped

    selftest_restore_current(self);
    return ok ? 0 : -1;
}

static volatile uint64_t g_kill_counter;
static volatile bool     g_kill_stop;   // set only as a safety net if process_kill somehow didn't work

static void kill_kthread_entry(void) {
    while (!g_kill_stop) {
        g_kill_counter++;
    }
    process_exit_self(0);
}

/* Proves the kill is (a) effective even against a process that never
 * cooperates (kill_kthread_entry never checks anything but g_kill_stop,
 * which the test never actually sets on the success path) and (b)
 * immediate: killing a process other than current_process reaps it
 * synchronously (see process_kill()'s comment), so the PCB slot is already
 * PROCESS_UNUSED the instant process_kill() returns — no need to wait for
 * a later schedule() call the way a self-exit does. */
int process_test_kill(void) {
    struct process *self = selftest_become_current();
    if (!self) {
        return -1;
    }

    g_kill_counter = 0;
    g_kill_stop = false;
    struct process *t = process_create_kthread(kill_kthread_entry);
    bool ok = (t != NULL);

    if (ok) {
        uint32_t pid = t->pid;
        selftest_wait_ticks(5);
        ok = g_kill_counter > 0;   // it actually ran before we killed it
        kprintf("process_test_kill: pre-kill counter=%u\n",
                (unsigned int)g_kill_counter);

        process_kill(pid);
        ok = ok && (t->state == PROCESS_UNUSED);   // reaped synchronously

        uint64_t after_kill = g_kill_counter;
        selftest_wait_ticks(5);
        ok = ok && (g_kill_counter == after_kill);   // and it truly stopped running
    }

    g_kill_stop = true;   // in case ok == false and it's still spinning somewhere
    selftest_restore_current(self);
    return ok ? 0 : -1;
}

static void reap_kthread_entry(void) {
    process_exit_self(0);   // do nothing but immediately exit
}

/* Proves Bug 3 (see docs/architecture/process-and-scheduling.md §16) stays
 * fixed: create-run-exit a kthread MAX_PROCESSES+4 times in a row, each one
 * fully reaped before the next is created. Before the fix, the (MAX_PROCESSES
 * + 1)th process_create_kthread() call here would have returned NULL
 * forever — the table permanently "full" of unreclaimed corpses. */
int process_test_reap(void) {
    struct process *self = selftest_become_current();
    if (!self) {
        return -1;
    }

    bool ok = true;
    int completed = 0;
    for (int i = 0; i < MAX_PROCESSES + 4; i++) {
        struct process *t = process_create_kthread(reap_kthread_entry);
        if (!t) {
            ok = false;
            break;
        }
        selftest_wait_ticks(4);   // let it run, self-exit, and get reaped
        completed++;
    }

    kprintf("process_test_reap: %d/%d create-run-exit cycles completed\n",
            completed, MAX_PROCESSES + 4);

    selftest_restore_current(self);
    return ok ? 0 : -1;
}

/* Verifies the guard page vmm_alloc_kernel_stack() places below every
 * kernel stack is genuinely unmapped, WITHOUT actually triggering an
 * overflow: this kernel's exception handler halts the whole system on any
 * unhandled #PF (no fault-recovery/fixup path exists yet — see
 * docs/architecture/process-and-scheduling.md's Failure Modes section), so
 * a live overflow can't be "caught" and reported as a clean pass/fail the
 * way the other three tests are. Checking that the page is unmapped is the
 * safe proxy: it's the property that guarantees a deep-enough overflow
 * WOULD fault there rather than silently corrupting adjacent memory. */
int process_test_stackguard(void) {
    uint64_t kstack_top = vmm_alloc_kernel_stack(KSTACK_SIZE);
    if (!kstack_top) {
        return -1;
    }

    uint64_t stack_base = kstack_top - KSTACK_SIZE;
    uint64_t guard_page = stack_base - PAGE_SIZE;

    bool guard_unmapped = (vmm_get_phys(guard_page) == 0);
    bool stack_itself_mapped = (vmm_get_phys(stack_base) != 0);   // sanity check

    kprintf("process_test_stackguard: guard page %s, stack base %s\n",
            guard_unmapped ? "unmapped (correct)" : "MAPPED (bug!)",
            stack_itself_mapped ? "mapped (correct)" : "UNMAPPED (bug!)");

    vmm_free_kernel_stack(kstack_top, KSTACK_SIZE);

    return (guard_unmapped && stack_itself_mapped) ? 0 : -1;
}



