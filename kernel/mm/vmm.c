#include "vmm.h"
#include "pmm.h"
#include "../drivers/serial.h"
#include "../cpu/spinlock.h"
#include <stdint.h>


static uint64_t kernel_pml4_phys = 0;     // physical address of the kernel PML4
static int vmm_direct_map_active = 0;     // 0 during early init, 1 after the CR3 switch
extern uint8_t kernel_end[];

/* Guards every page-table STRUCTURE mutation (vmm_map_in/vmm_unmap/
 * vmm_create_address_space/vmm_destroy_address_space, all of which walk or
 * rewrite shared PML4/PDPT/PD/PT pages via get_or_create_table()/
 * vmm_destroy_table()) plus the kstack/MMIO bump-pointer VA reservations
 * below. Unlike pmm_lock/heap_lock (added alongside this one, same reason),
 * this one was missing when Phase 3's SMP scheduler integration first
 * landed: process_create_kthread() calls vmm_alloc_kernel_stack() ->
 * vmm_map() with NO g_sched_lock held (proc_alloc() already released it),
 * while process_reap_slot() calls vmm_free_kernel_stack()/
 * vmm_destroy_address_space() -> vmm_unmap() WITH g_sched_lock held -- two
 * DIFFERENT locks (one of them nonexistent), so a kthread creation on one
 * core and a reap on another could walk/rewrite the exact same shared
 * page-table page at the same time. Observed directly: a double fault
 * inside vmm_flush_tlb() (a bare invlpg) with a live, non-zero RBP --
 * config consistent with corrupted page-table structure, not a fresh/
 * never-run context (which is the OTHER class of bug already fixed
 * elsewhere in this file's callers). */
static spinlock_t vmm_lock = SPINLOCK_INIT;


// Map a physical page-table page to a usable virtual pointer.
//
// Page-table pages are handed out by the PMM from anywhere in usable RAM, so
// they routinely live past the kernel image. Before the CR3 switch the only
// active mapping that spans arbitrary low RAM is the stage2 kernel window
// (KP2V covers the low 1GB). After the switch that window only covers the
// kernel image (up to ~2MB), so we must reach page-table pages through the
// direct map (P2V), which spans all usable RAM. Using KP2V post-switch is the
// bug that faulted once the kernel grew enough to push allocations past 2MB.
static inline uint64_t *vmm_table(uint64_t phys) {
    return (uint64_t *)(vmm_direct_map_active ? P2V(phys) : KP2V(phys));
}
static inline uint64_t *vmm_pml4(void) { return vmm_table(kernel_pml4_phys); }





// helper functions
static inline uint64_t pml4_index(uint64_t v) { return (v >> 39) & 0x1ff; }
static inline uint64_t pdpt_index(uint64_t v) { return (v >> 30) & 0x1ff; }
static inline uint64_t pd_index(uint64_t v) { return (v >> 21) & 0x1ff; }
static inline uint64_t pt_index(uint64_t v) { return (v >> 12) & 0x1ff; }

// Allocation and zero a new table
static uint64_t *alloc_table(void) {

    uint64_t pyhs_addr = pmm_alloc_page();
    if (!pyhs_addr){
        return 0;
    }
    uint64_t *table = (uint64_t *)vmm_table(pyhs_addr);
    for (int i = 0; i < 512; i++) {
        table[i] = 0;
    }
    return table;
}

/* Create a fresh address space: a new PML4 whose USER half(slots 0-255) is empty 
 and the KERNEL half (slots 256-511) is identity-mapped. returns the physical address of the new PML4. */
uint64_t vmm_create_address_space(void) {
    uint64_t pml4_phys = pmm_alloc_page();
    if (!pml4_phys) {
        return 0;
    }

    spin_lock(&vmm_lock);

    uint64_t *pml4 = (uint64_t *)vmm_table(pml4_phys);    // virtual pointer to the new PML4
    uint64_t *kernel_pml4 = vmm_pml4(); // virtual pointer to the kernel's PML4

    for (int i = 0; i < 512; i++) {
        pml4[i] = 0;                    // empty user half + identity-map kernel half
    }

    for (int i = 256; i < 512; i++) {
        pml4[i] = kernel_pml4[i];      // copy kernel half from the current PML4
    }

    spin_unlock(&vmm_lock);
    return pml4_phys;
}

static void vmm_destroy_table(uint64_t table_phys, int level) {
    uint64_t *table = vmm_table(table_phys);

    for (int i = 0; i < 512; i++) {
        uint64_t entry = table[i];
        if (!(entry & VMM_PRESENT)) {
            continue;
        }

        uint64_t child_phys = entry & VMM_ADDR_MASK;

        if (level == 1) {
            pmm_free_page(child_phys);
            continue;
        }

        if (entry & VMM_HUGE) {
            serial_write_string("vmm_destroy_address_space: huge user mapping unsupported\n");
            continue;
        }

        vmm_destroy_table(child_phys, level - 1);
    }

    pmm_free_page(table_phys);
}

void vmm_destroy_address_space(uint64_t pml4_phys) {
    if (!pml4_phys || pml4_phys == kernel_pml4_phys) {
        return;
    }

    spin_lock(&vmm_lock);

    uint64_t *pml4 = vmm_table(pml4_phys);
    for (int i = 0; i < 256; i++) {
        uint64_t entry = pml4[i];
        if (!(entry & VMM_PRESENT)) {
            continue;
        }

        vmm_destroy_table(entry & VMM_ADDR_MASK, 3);
        pml4[i] = 0;
    }

    spin_unlock(&vmm_lock);
    pmm_free_page(pml4_phys);
}


/* Switch the active address space by loading CR3 with a new PML4 physical address */
void vmm_switch_address_space(uint64_t pml4_phys) {
    __asm__ volatile("mov %0, %%cr3" : : "r"(pml4_phys) : "memory");
}
// Get or Create the next-level table from entry
static uint64_t *get_or_create_table(uint64_t *parent, uint64_t index, uint64_t flags) {

    if (parent[index] & VMM_PRESENT) {
        // Table already exists, return its vitual address
        uint64_t phys_addr = parent[index] & VMM_ADDR_MASK;
        return (uint64_t *)vmm_table(phys_addr);
    }
    // Allocate a new table and set its flags
    uint64_t phys_addr = pmm_alloc_page();
    if (!phys_addr) {
        return 0;
    }

    uint64_t *table = (uint64_t *)vmm_table(phys_addr);
    for (int i = 0; i < 512; i++) {
        table[i] = 0;
    }

    // install in parent table
    parent[index] = phys_addr | VMM_PRESENT | VMM_WRITABLE | flags;
    return table;
}


/* --- Refactor: walkers operate on a GIVEN pml4 physical address --- */
/* The core mapper, now parameterized by the target address space's PML4.
 `pml4_phys` is the physical address of the PML4 to map into the table;
 vmm_table() turns a physical address into a virtual pointer regardless of 
 which address space it belongs to (page-table pages are reachable via the
 direct map no matter whose tree they're in). */
 int vmm_map_in(uint64_t pml4_phys, uint64_t virt_addr, uint64_t phys_addr, uint64_t flags){
    spin_lock(&vmm_lock);

    uint64_t *pml4 = vmm_table(pml4_phys);

    uint64_t *pdpt = get_or_create_table(pml4, pml4_index(virt_addr),  flags);
    if (!pdpt) {
        spin_unlock(&vmm_lock);
        return -1;
    }

    uint64_t *pd = get_or_create_table(pdpt, pdpt_index(virt_addr), flags);
    if (!pd) {
        spin_unlock(&vmm_lock);
        return -1;
    }

    uint64_t *pt = get_or_create_table(pd, pd_index(virt_addr), flags);
    if (!pt) {
        spin_unlock(&vmm_lock);
        return -1;
    }

    // set the page table entry
    uint64_t page_index = pt_index(virt_addr); // 12 bits for 4K pages
    pt[page_index] = (phys_addr & VMM_ADDR_MASK )| VMM_PRESENT | flags;
    vmm_flush_tlb(virt_addr);// flush TLB to update mapping /* harmless if the target address space isn't active */

    spin_unlock(&vmm_lock);
    return 0;
 }


 // Public API
int vmm_map(uint64_t virt_addr, uint64_t phys_addr, uint64_t flags) {
    return vmm_map_in(kernel_pml4_phys, virt_addr, phys_addr, flags);
}


uint64_t vmm_get_kernel_pml4(void) {
    return kernel_pml4_phys;
}


void vmm_unmap(uint64_t virt_addr){
    spin_lock(&vmm_lock);

    uint64_t *pml4 = vmm_pml4();
    if (!(pml4[pml4_index(virt_addr)] & VMM_PRESENT)) {
        spin_unlock(&vmm_lock);
        return;
    }
    uint64_t *pdpt = (uint64_t *)vmm_table(pml4[pml4_index(virt_addr)] & VMM_ADDR_MASK);

    if (!(pdpt[pdpt_index(virt_addr)] & VMM_PRESENT)) {
        spin_unlock(&vmm_lock);
        return;
    }
    uint64_t *pd = (uint64_t *)vmm_table(pdpt[pdpt_index(virt_addr)] & VMM_ADDR_MASK);

    if (!(pd[pd_index(virt_addr)] & VMM_PRESENT)) {
        spin_unlock(&vmm_lock);
        return;
    }
    uint64_t *pt = (uint64_t *)vmm_table(pd[pd_index(virt_addr)] & VMM_ADDR_MASK);

    uint64_t page_index = pt_index(virt_addr); // represents 12 bits for 4K pages
    pt[page_index] = 0; // clear the page table entry
    vmm_flush_tlb(virt_addr); // flush TLB to update mapping

    spin_unlock(&vmm_lock);
}

/* Look up the physical address a VA maps to IN A GIVEN address space.
   Mirrors vmm_get_phys but walks `pml4_phys` instead of the kernel PML4 —
   needed so elf_load can find the frame it just mapped into the process
   space and reach it via the direct map. */
uint64_t vmm_get_phys_in(uint64_t pml4_phys, uint64_t virt_addr) {
    uint64_t *pml4 = vmm_table(pml4_phys);
    if (!(pml4[pml4_index(virt_addr)] & VMM_PRESENT)) return 0;
    uint64_t *pdpt = vmm_table(pml4[pml4_index(virt_addr)] & VMM_ADDR_MASK);
    if (!(pdpt[pdpt_index(virt_addr)] & VMM_PRESENT)) return 0;
    uint64_t *pd = vmm_table(pdpt[pdpt_index(virt_addr)] & VMM_ADDR_MASK);
    if (!(pd[pd_index(virt_addr)] & VMM_PRESENT)) return 0;
    uint64_t *pt = vmm_table(pd[pd_index(virt_addr)] & VMM_ADDR_MASK);
    if (!(pt[pt_index(virt_addr)] & VMM_PRESENT)) return 0;
    return (pt[pt_index(virt_addr)] & VMM_ADDR_MASK) | (virt_addr & 0xfff);
}

/* Existing public one becomes the kernel-space wrapper. */
uint64_t vmm_get_phys(uint64_t virt_addr) {
    return vmm_get_phys_in(kernel_pml4_phys, virt_addr);
}


void vmm_flush_tlb(uint64_t virt_addr) {
    __asm__ volatile("invlpg (%0)" : : "r" (virt_addr) : "memory");
}



/* EFER.NXE (bit 11) is a PER-CORE MSR, not a paging-structure-wide setting:
 * every core that ever walks a page table with VMM_NX (bit 63) set in any
 * entry needs its OWN EFER.NXE=1, or that core treats bit 63 as a plain
 * reserved bit and takes a reserved-bit page fault the moment it touches
 * that mapping -- regardless of what any other core's EFER says. The BSP
 * gets this from vmm_init() below; every AP must call this too (ap_main(),
 * kernel/cpu/smp.c) before it can safely touch ANY VMM_NX-mapped memory,
 * which in practice means immediately (kernel stacks are mapped
 * VMM_WRITABLE | VMM_NX). Missing this on the APs was a real bug: it
 * surfaced as a double fault (escalated from an unhandled reserved-bit
 * #PF) the first time an AP's schedule_locked() switched into a fresh
 * kthread's stack. */
void vmm_enable_nx_this_cpu(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000080));
    lo |= (1 << 11);                        /* NXE */
    __asm__ volatile("wrmsr" : : "c"(0xC0000080), "a"(lo), "d"(hi));
}

// Initialize the virtual memory manager
void vmm_init(void) {
    serial_write_string("\n=== VMM init ===\n");

    vmm_enable_nx_this_cpu();

    // step 1:Allocate a new page for the kernel PML4
    // alloc_table() runs in the early phase (vmm_direct_map_active == 0), so it
    // hands back a KP2V pointer; recover the physical address with KV2P.
    uint64_t *pml4 = alloc_table();
    if (!pml4) {
        serial_write_string("FATAL: Could not allocate PML4\n");
        while (1) {}
    }
    kernel_pml4_phys = KV2P(pml4);

    serial_write_string("PML4 Allocated at phys: ");
    serial_write_hex(kernel_pml4_phys);
    serial_write_string("\n");


    // step 2: Map 0 to 1GB physical memory to higher half virtual memory
    // Map all usable physical RAM E820 using 2 MB pages
    // for simplicity, we map from 0 to the highest usable address
    // ... etc 1GB
    uint32_t e820_count = *(uint32_t *)KP2V(E820_COUNT_ADDR);
    struct e820_entry *e820_entries = (struct e820_entry *)KP2V(E820_ENTRIES_ADDR);

    uint64_t highest_phys = 0;
    for (uint64_t i = 0; i < e820_count; i++) {
        if (e820_entries[i].type != E820_USABLE) {
            continue;
        }
        uint64_t end = e820_entries[i].base + e820_entries[i].length;
        if (end > highest_phys) {
            highest_phys = end;
        }
    }

    // Rounded up to nearest 2MB boundary
    highest_phys = (highest_phys + 0x1fffff) & ~0x1fffffULL; // 2MB pages

    serial_write_string("Building direct map from 0 to phys: ");
    serial_write_hex(highest_phys);
    serial_write_string("(2MB pages)\n");

    for (uint64_t phys = 0; phys < highest_phys; phys += 0x200000) {
        uint64_t virt = phys + DIRECT_MAP_BASE;
        
        uint64_t *pdpt = get_or_create_table(vmm_pml4(), pml4_index(virt),  VMM_WRITABLE);
        if (!pdpt) {
            serial_write_string("FATAL: Could not allocate PDPT\n");
            while (1) {}
        }

        uint64_t *pd = get_or_create_table(pdpt, pdpt_index(virt), VMM_WRITABLE);
        if (!pd) {
            serial_write_string("FATAL: Could not allocate PD\n");
            while (1) {}
        }

        uint64_t page_index = pd_index(virt); // 21 bits for 2MB pages
        pd[page_index] = (phys & ~0x1fffffULL) | VMM_PRESENT | VMM_WRITABLE | VMM_HUGE;
    }
       
    
    serial_write_string("Mapping complete\n");

    // step 3: build the kernel mapping at KERNEL_VIRTUAL_BASE
    // Map [0 .. kernel image + PMM bitmap) so the kernel can grow past 2 MB
    // without faulting after the CR3 switch. The boot stack now lives in the
    // kernel's own .bss (see kernel/cpu/kentry.asm), so it is below kernel_end
    // and covered automatically. The PMM bitmap sits *at* kernel_end and is
    // written before this remap, so extend the mapping to pmm_reserved_phys_end()
    // — otherwise the first bitmap access after the CR3 switch page-faults.
    uint64_t kernel_end_phys = (KV2P(kernel_end) + (PAGE_SIZE - 1)) & ~(PAGE_SIZE - 1);
    uint64_t kernel_map_end = pmm_reserved_phys_end();
    if (kernel_map_end < kernel_end_phys)
        kernel_map_end = kernel_end_phys;

    serial_write_string("Mapping Kernel at 0xFFFFFFFF80000000 up to phys: ");
    serial_write_hex(kernel_map_end);
    serial_write_string("\n");
    for (uint64_t phys = 0; phys < kernel_map_end; phys += PAGE_SIZE) {
        uint64_t virt = KERNEL_VIRTUAL_BASE + phys;
        if (vmm_map(virt, phys, VMM_WRITABLE) < 0) {
            serial_write_string("FATAL: vmm_map kernel failed at\n");
            serial_write_hex(phys);
            serial_write_string("\n");
            while (1) {}
    
        }

    }
    serial_write_string("Kernel mapping complete\n");


    // step 4: switch CR3 to point to the kernel PML4 (new page tables)

    serial_write_string("switching CR3 ...\n");
    uint64_t new_cr3 = kernel_pml4_phys;
    __asm__ volatile("mov %0, %%cr3" : : "r" (new_cr3): "memory");
    // The new tables carry the full direct map, so from here on page-table
    // pages must be reached through it (P2V) rather than the now-2MB kernel
    // window (KP2V).
    vmm_direct_map_active = 1;
    serial_write_string("CR3 set to ");
    serial_write_hex(new_cr3);
    serial_write_string("\n");
}

// Bump pointer for MMIO Virtual address allocation
static uint64_t mmio_next_virt = MMIO_BASE;

uint64_t vmm_map_mmio(uint64_t phys_addr, uint64_t size) {
    // Round up to nearest 4KB boundary
    uint64_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    
    // Page-align the physical address downwards, remember the offset
    uint64_t phys_algned = phys_addr & ~0xFFFULL; // 4KB pages
    uint64_t offset = phys_addr - phys_algned;

    // If the offset pushes us into the next page, we need to allocate a new page
    uint64_t total_size = (size + PAGE_SIZE + offset -1) & ~0xFFFULL; 
    pages = total_size  / PAGE_SIZE;

    // Reserve the virtual address range
    spin_lock(&vmm_lock);
    uint64_t virt_base = mmio_next_virt;
    mmio_next_virt += total_size;
    spin_unlock(&vmm_lock);

    serial_write_string("vmm_map_mmio: phys=");
    serial_write_hex(phys_algned);
    serial_write_string(", virt=");
    serial_write_hex(virt_base);
    serial_write_string(", pages=");
    serial_write_hex(pages);
    serial_write_string(", \n");

    // Map the pages
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t virt = virt_base + (i * PAGE_SIZE);
        uint64_t phys = phys_algned + (i * PAGE_SIZE);
        if (vmm_map(virt, phys, VMM_WRITABLE | VMM_NOCACHE) < 0) {
            serial_write_string("FATAL: vmm_map mmio failed at\n");
            serial_write_hex(phys);
            return 0;
        }
    }

    // Return the virtual address of the first page
    return virt_base + offset;
}

// Bump pointer for kernel-stack VA allocation.
//
// This MUST land in a PML4 slot that's already populated in the kernel's own
// page tables before the *first* process address space is ever created:
// vmm_create_address_space() shares the kernel half by copying PML4 entries
// *by value* at creation time (kernel_pml4.c: `pml4[i] = kernel_pml4[i]` for
// i in [256,512)) — a one-time snapshot, not a live reference. A PML4 slot
// that's still empty (not-present) at that moment copies as not-present
// forever, even after the kernel's own table later fills it in; a region
// carved out of genuinely fresh address space (tried first: a dedicated
// 0xFFFFB0... base) hits exactly this — the first process's own stack
// mapping, created moments after its address space snapshot, is invisible
// in its own page tables. #PF on first touch, which then can't even push
// its own fault frame (same unmapped region backs TSS.RSP0) -> #DF.
//
// Fix: reuse MMIO_BASE's PML4 slot instead of a new one. HPET/LAPIC/IOAPIC/
// the framebuffer all call vmm_map_mmio() during early boot, long before any
// process exists, which guarantees that slot is already present by the time
// the first vmm_create_address_space() runs. Offset well clear (256 GiB in,
// out of the 512 GiB one PML4 entry spans) of where the actual MMIO bump
// allocator grows (a few hundred KB at most).
#define KSTACK_REGION_BASE (MMIO_BASE + (1ULL << 38))
static uint64_t kstack_next_virt = KSTACK_REGION_BASE;

uint64_t vmm_alloc_kernel_stack(uint64_t size) {
    uint64_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;

    // Reserve a guard page, then the stack pages right above it. The guard
    // page is deliberately left unmapped: a stack that overflows downward
    // walks straight into it and takes a #PF at the overflow site instead of
    // silently corrupting whatever memory used to sit there.
    spin_lock(&vmm_lock);
    uint64_t guard_virt = kstack_next_virt;
    uint64_t stack_base_virt = guard_virt + PAGE_SIZE;
    kstack_next_virt = stack_base_virt + (pages * PAGE_SIZE);
    spin_unlock(&vmm_lock);

    for (uint64_t i = 0; i < pages; i++) {
        uint64_t phys = pmm_alloc_page();
        if (!phys) {
            serial_write_string("FATAL: vmm_alloc_kernel_stack: out of physical memory\n");
            return 0;
        }
        uint64_t virt = stack_base_virt + (i * PAGE_SIZE);
        if (vmm_map(virt, phys, VMM_WRITABLE | VMM_NX) < 0) {
            serial_write_string("FATAL: vmm_alloc_kernel_stack: vmm_map failed\n");
            pmm_free_page(phys);
            return 0;
        }
    }

    return stack_base_virt + (pages * PAGE_SIZE);  // top of stack (RSP seed)
}

void vmm_free_kernel_stack(uint64_t stack_top, uint64_t size) {
    uint64_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t stack_base_virt = stack_top - (pages * PAGE_SIZE);

    for (uint64_t i = 0; i < pages; i++) {
        uint64_t virt = stack_base_virt + (i * PAGE_SIZE);
        uint64_t phys = vmm_get_phys(virt);
        if (phys) {
            pmm_free_page(phys);
        }
        vmm_unmap(virt);
    }
    // The VA range (including the guard page below it) is intentionally not
    // reclaimed — same bump-allocator trade-off vmm_map_mmio already makes.
}