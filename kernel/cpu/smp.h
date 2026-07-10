#ifndef __SMP_H__
#define __SMP_H__

#include <stdint.h>

/* Physical address the AP (application processor) real-mode trampoline is
 * copied to before sending the SIPI (Startup IPI) that tells each AP to
 * begin executing there. Requirements on this address (Intel SDM Vol. 3A
 * §8.4): must be page-aligned and below 1MB, since the SIPI vector encodes
 * only the PAGE NUMBER (address >> 12) of where a still-real-mode core
 * starts fetching instructions from -- CS:IP = vector<<8 : 0000.
 *
 * 0x8000 is deliberately clear of everything else this bootloader/kernel
 * still cares about at the time smp_bringup() runs (well after boot):
 * the E820 buffer (0x7000-0x7004), the VBE info block (0x5000-0x53FF), and
 * the bootloader's own temporary page tables (0x9000+, moot after vmm_init
 * rebuilds and switches CR3, but avoided anyway for clarity). See
 * pmm_reserve_page()'s call site in pmm.c for why this is reserved
 * explicitly rather than relied upon incidentally. */
#define AP_TRAMPOLINE_PHYS 0x8000UL

/* Permanently identity-maps physical [0, 1MB) into the kernel's own PML4
 * (VMM_WRITABLE, no VMM_USER -- so vmm_create_address_space()'s zeroing of
 * the user-half PML4 slots keeps this invisible to every real ring-3
 * process, see that function's comment). Without this, an AP's trampoline
 * (kernel/cpu/ap_trampoline.asm) would page-fault the instant it enables
 * paging with CR3 = kernel_pml4_phys while still executing at its low
 * physical address -- there is no other identity mapping anywhere in this
 * kernel's page tables (kernel/mm/vmm.c only builds the direct map and the
 * higher-half kernel mapping). Called once from kernel_main(), after
 * vmm_init() and before smp_bringup(). */
void ap_bootstrap_map(void);

/* Bring up every AP found in the MADT (via acpi_get_info(), already
 * enumerated by percpu_init_topology()) using INIT-SIPI-SIPI
 * (kernel/cpu/lapic.c's lapic_start_ap()). For each AP: pokes that core's
 * own stack-top and ap_entry64's address into the trampoline's data slots
 * (kernel/cpu/ap_trampoline.asm), sends the sequence, then polls
 * cpu_table[i].online (kernel/cpu/percpu.h) with a timeout before moving
 * to the next core -- APs are started one at a time, not in parallel,
 * since they all share the one trampoline page. Must run after
 * ap_bootstrap_map(), percpu_init_topology(), and idt_init(). */
void smp_bringup(void);

/* C entry point an AP reaches via kernel/cpu/ap_entry.asm's ap_entry64,
 * once it has a valid higher-half stack (poked by smp_bringup() before
 * this core's SIPI) but nothing else set up yet. Builds this core's own
 * GDT/TSS (gdt_init_this_cpu()), loads the shared IDT
 * (idt_load_this_cpu()), enables this core's own LAPIC
 * (lapic_init_this_cpu()), marks cpu_table[this].online = true, then
 * parks in a halt loop -- deliberately not touching the scheduler yet
 * (see docs/architecture/process-and-scheduling.md's SMP phase for why
 * that's a separate, later step). Never returns. */
void ap_main(void);

#endif /* __SMP_H__ */
