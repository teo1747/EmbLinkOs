#ifndef __PERCPU_H__
#define __PERCPU_H__

#include <stdint.h>
#include "include/types.h"
#include "arch/x86_64/cpu/gdt.h"

struct thread;    // forward decls only -- percpu.h must not depend on process.h
struct process;   // (process.h includes this header, to get current_thread's
                  // per-CPU home; a cycle back the other way would break)

/* MAX_CPUS is defined in gdt.h (included above), not here -- see that
 * header's comment for why (avoids a circular include, since GDT_ENTRIES
 * there also needs a CPU count). */

/* Everything that used to be a single kernel-wide global, now one instance
 * per core (docs/architecture/process-and-scheduling.md's SMP phase).
 * cpu_table[0] is always the BSP (boot strap processor) -- cpu_index is
 * assigned in MADT enumeration order by percpu_init_topology(), and the
 * BSP is always first in that order because it's the one that read the
 * MADT in the first place. */
struct cpu_data {
    uint32_t apic_id;
    uint32_t cpu_index;

    /* Was gdt.c's single static g_tss/rsp0_stack/df_stack. One real TSS per
     * core: each core takes its own ring-3->ring-0 interrupts on its own
     * kernel stack, so TSS.RSP0 (and the #DF IST1 stack) cannot be shared. */
    struct tss tss;
    uint8_t rsp0_stack[16 * 1024] __attribute__((aligned(16)));
    uint8_t df_stack[16 * 1024]   __attribute__((aligned(16)));

    /* Was process.c's single global `current_process` / `g_pending_reap`.
     * See process.h's current_thread/current_process macros, which read
     * through here. Phase 4 (docs/architecture/process-and-scheduling.md):
     * the schedulable unit is now a thread, not a process -- current_process
     * is derived from current_thread->proc, not stored separately. */
    struct thread *current_thread;

    /* Deferred reap, two levels (Phase 4): pending_thread_reap is the
     * thread whose kernel stack this core just switched away from (freed
     * next tick, same reasoning as before the split). pending_process_reap
     * is only set alongside it when that thread's exit ALSO completed its
     * owning process (its last live thread) with no parent to hand off
     * to -- then the process's address space is freed at the same next-
     * tick moment. See schedule_locked()'s comment (process.c) for why
     * both must wait until this core is provably off the stack/CR3. */
    struct thread *pending_thread_reap;
    struct process *pending_process_reap;

    bool online;   /* set true by ap_main() once this core has a working
                     * C environment (own GDT/TSS/IDT/LAPIC) -- checked by
                     * smp_bringup() (kernel/cpu/smp.c) to know an AP made
                     * it, and by `test smp online`. Always true for
                     * cpu_table[0] (the BSP), set at percpu_init_topology()
                     * time since it's already running by definition. */
};

extern struct cpu_data cpu_table[MAX_CPUS];

/* Number of entries in cpu_table[] that are actually populated (BSP + every
 * AP found in the MADT) -- NOT all of MAX_CPUS. Set once by
 * percpu_init_topology(). */
extern uint32_t cpu_count;

/* Build cpu_table[]/the APIC-ID reverse lookup from ACPI's MADT CPU list
 * (acpi_get_info()). Must run after BOTH acpi_init() (needs cpu_apic_ids[])
 * and lapic_init() (needs lapic_get_id() to work, to identify the BSP's own
 * entry) -- see kernel_main's call site. Marks cpu_table[0] (the BSP)
 * online immediately; every AP marks itself online later, in ap_main(). */
void percpu_init_topology(void);

/* Returns this core's own struct cpu_data, found via its LAPIC ID. Safe to
 * call as soon as percpu_init_topology() has run; before that (i.e. only
 * during the BSP's own very first gdt_init_bsp() call, which happens
 * before acpi_init()/lapic_init()), falls back to cpu_table[0] -- correct
 * by construction, since nothing but the BSP itself is executing at that
 * point anyway. */
struct cpu_data *this_cpu(void);

static inline bool this_cpu_is_bsp(void) {
    return this_cpu()->cpu_index == 0;
}

#endif /* __PERCPU_H__ */
