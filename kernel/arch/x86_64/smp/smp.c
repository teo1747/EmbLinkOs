#include "arch/x86_64/smp/smp.h"
#include "arch/x86_64/cpu/percpu.h"
#include "arch/x86_64/cpu/gdt.h"
#include "arch/x86_64/irq/idt.h"
#include "arch/x86_64/irq/lapic.h"
#include "arch/x86_64/cpu/fpu.h"
#include "mm/vmm.h"
#include "mm/pmm.h"
#include "acpi/acpi.h"
#include "process/process.h"
#include "include/kprintf.h"
#include "include/kstring.h"

/* The trampoline blob (kernel/cpu/ap_trampoline.asm, assembled separately
 * with `nasm -f bin` per the Makefile) embedded into the kernel image as a
 * plain byte array by kernel/cpu/ap_trampoline_blob.asm's `incbin`. These
 * two labels bracket it so C can find both its bytes and its length. */
extern const uint8_t ap_trampoline_blob_start[];
extern const uint8_t ap_trampoline_blob_end[];

/* The trampoline's three poke slots (ap_pml4_phys, ap_stack_top,
 * ap_entry_point -- see ap_trampoline.asm) are, BY CONSTRUCTION, the last
 * three 8-byte words in the blob, immediately before ap_trampoline_end --
 * nothing in that .asm file may ever be added after them. This lets C
 * locate them purely from the blob's total length, with no separate
 * offset bookkeeping to keep in sync by hand. */
#define AP_SLOT_ENTRY_POINT(base) ((volatile uint64_t *)((base) + (blob_size() - 8)))
#define AP_SLOT_STACK_TOP(base)   ((volatile uint64_t *)((base) + (blob_size() - 16)))
#define AP_SLOT_PML4_PHYS(base)   ((volatile uint64_t *)((base) + (blob_size() - 24)))

extern void ap_entry64(void);   // kernel/cpu/ap_entry.asm

static inline uint64_t blob_size(void) {
    return (uint64_t)(ap_trampoline_blob_end - ap_trampoline_blob_start);
}

void ap_bootstrap_map(void) {
    /* Identity-map [0, 1MB) into the kernel's own PML4: virt == phys,
     * writable, NOT VMM_USER (so vmm_create_address_space()'s zeroing of
     * the user-half PML4 slots keeps it invisible to every real process --
     * see that function's comment), NOT VMM_NX (the AP trampoline has to
     * EXECUTE out of this region). See smp.h's comment on why this exists
     * at all: without it, enabling paging with CR3 = kernel_pml4_phys
     * while still executing at a low physical address page-faults
     * immediately, since nothing else in this kernel's page tables maps
     * low physical addresses to themselves. */
    for (uint64_t phys = 0; phys < 0x100000; phys += PAGE_SIZE) {
        vmm_map(phys, phys, VMM_WRITABLE);
    }
    kprintf("smp: identity-mapped [0, 1MB) into the kernel PML4 for AP bring-up\n");
}

/* C entry point for an AP, reached via ap_entry64 (kernel/cpu/ap_entry.asm)
 * with a valid higher-half stack already in place (poked by smp_bringup()
 * below before this core's SIPI). */
void ap_main(void) {
    /* EFER.NXE is per-core -- must happen before this AP touches ANY
     * VMM_NX-mapped memory (its own kernel stack included). See
     * vmm_enable_nx_this_cpu()'s comment: a missing call here was a real
     * bug, not a defensive measure -- reserved-bit double faults the first
     * time an AP ran a fresh kthread off its NX-mapped stack. */
    vmm_enable_nx_this_cpu();
    fpu_init_this_cpu();   // per-core, same reasoning as vmm_enable_nx_this_cpu()
                            // just above -- see fpu.h. Must land before this
                            // core ever runs a kernel_ctx_switch() (its own
                            // idle kthread's adoption below counts).

    gdt_init_this_cpu();
    idt_load_this_cpu();
    lapic_init_this_cpu();

    struct cpu_data *me = this_cpu();

    kprintf("smp: AP apic_id=%u (index %u) reached ap_main\n",
            (unsigned int)me->apic_id, (unsigned int)me->cpu_index);

    /* Program this core's OWN LAPIC timer -- each core calibrates and
     * configures independently (see lapic_timer_init()'s comment on why
     * the shared lapic_timer_ticks_per_ms global being overwritten by
     * each core in turn is harmless: nothing reads it after its own
     * immediate use). This is what actually makes this core a scheduler
     * participant -- lapic_timer_handler() (lapic.c) calls schedule()
     * unconditionally on every tick, on every core, once each core's own
     * timer is live. */
    lapic_timer_init(48);

    /* Turn this execution context into a real, schedulable process --
     * exactly what process_adopt_current() already does for the BSP's own
     * shell (main.c) -- so this core has something for schedule() to
     * switch away from/back to instead of current_process being NULL on
     * this core forever. */
    if (!process_adopt_current()) {
        kprintf("smp: AP apic_id=%u failed to adopt a process -- scheduler unavailable on this core\n",
                (unsigned int)me->apic_id);
    }

    /* Only NOW -- after every step above that isn't itself locked
     * (lapic_timer_init() writes the shared IDT entry for vector 48;
     * kprintf/serial output has no locking of its own either, see below)
     * has fully finished -- mark this core online. smp_bringup() polls
     * `online` before starting the NEXT AP's SIPI; setting it any earlier
     * (as an initial version of this function did) lets the BSP begin
     * bringing up the next core while THIS one is still mid-setup, running
     * genuinely concurrently with no serialization at all yet -- observed
     * as garbled interleaved serial output and, from there, an actual
     * double fault (two cores' unsynchronized kprintf calls corrupting
     * shared state). APs are brought up strictly one at a time
     * specifically to avoid this; this flag is the handshake that makes
     * that actually true, not just intended. */
    me->online = true;

    __asm__ volatile ("sti");
    /* This core's own idle loop: hlt until the next interrupt (its own
     * timer tick, or any device IRQ IO-APIC ever routes here). Every timer
     * tick's schedule() call may switch this core away to run whatever
     * else is READY across the whole (single, global-locked) proc_table,
     * and back again later -- the same mechanism that already drives
     * preemption on the BSP, now running independently on every core. */
    for (;;) {
        __asm__ volatile ("hlt");
    }
}

void smp_bringup(void) {
    kprintf("\n=== SMP bring-up ===\n");

    if (cpu_count <= 1) {
        kprintf("smp: single core, nothing to bring up\n");
        return;
    }

    uint64_t trampoline_virt = P2V(AP_TRAMPOLINE_PHYS);

    for (uint32_t i = 1; i < cpu_count; i++) {
        struct cpu_data *ap = &cpu_table[i];

        /* Fresh copy of the trampoline for every AP: the previous AP's own
         * bring-up may have left the poked data slots (and, in principle,
         * self-modifying real-mode state) pointing at ITS values -- APs
         * are started strictly one at a time (see the loop structure), so
         * reusing the one trampoline page between them is safe as long as
         * each iteration re-copies and re-pokes before its own SIPI. */
        memcpy((void *)trampoline_virt, ap_trampoline_blob_start, blob_size());

        *AP_SLOT_PML4_PHYS(trampoline_virt)   = vmm_get_kernel_pml4();
        *AP_SLOT_STACK_TOP(trampoline_virt)   = (uint64_t)(ap->rsp0_stack + sizeof(ap->rsp0_stack));
        *AP_SLOT_ENTRY_POINT(trampoline_virt) = (uint64_t)(uintptr_t)ap_entry64;

        kprintf("smp: starting AP apic_id=%u (index %u)...\n",
                (unsigned int)ap->apic_id, (unsigned int)i);

        lapic_start_ap(ap->apic_id, AP_TRAMPOLINE_PHYS);

        /* Bounded wait, not indefinite: an AP that never reports in (wrong
         * SIPI vector encoding, a bug in the trampoline, hardware that
         * doesn't support MP bring-up the way this code assumes) must not
         * hang the BSP's entire boot -- log it and move on to the next
         * core rather than getting stuck forever. */
        bool came_online = false;
        for (int spin = 0; spin < 2000000; spin++) {
            if (ap->online) {
                came_online = true;
                break;
            }
            __asm__ volatile ("pause");
        }

        if (!came_online) {
            kprintf("smp: AP apic_id=%u did NOT come online (timed out)\n",
                    (unsigned int)ap->apic_id);
        }
    }

    kprintf("=== SMP bring-up done ===\n");
}
