#include "vmm.h"
#include "pmm.h"
#include "../drivers/serial.h"
#include <stdint.h>


static uint64_t *kernel_pml4 = 0;


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
    uint64_t *table = (uint64_t *)KP2V(pyhs_addr);
    for (int i = 0; i < 512; i++) {
        table[i] = 0;
    }
    return table;
}
 
// Get or Create the next-level table from entry
static uint64_t *get_or_create_table(uint64_t *parent, uint64_t index, uint64_t flags) {

    if (parent[index] & VMM_PRESENT) {
        // Table already exists, return its vitual address
        uint64_t phys_addr = parent[index] & VMM_ADDR_MASK;
        return (uint64_t *)KP2V(phys_addr);
    }
    // Allocate a new table and set its flags
    uint64_t phys_addr = pmm_alloc_page();
    if (!phys_addr) {
        return 0;
    }
    
    uint64_t *table = (uint64_t *)KP2V(phys_addr);
    for (int i = 0; i < 512; i++) {
        table[i] = 0;
    }

    // install in parent table
    parent[index] = phys_addr | VMM_PRESENT | VMM_WRITABLE | flags;
    return table;
}


// Public API
int vmm_map(uint64_t virt_addr, uint64_t phys_addr, uint64_t flags) {
    uint64_t *pdpt = get_or_create_table(kernel_pml4, pml4_index(virt_addr),  flags);
    if (!pdpt) {
        return -1;
    }

    uint64_t *pd = get_or_create_table(pdpt, pdpt_index(virt_addr), flags);
    if (!pd) {
        return -1;
    }

    uint64_t *pt = get_or_create_table(pd, pd_index(virt_addr), flags);
    if (!pt) {
        return -1;
    }

    // set the page table entry
    uint64_t page_index = pt_index(virt_addr); // 12 bits for 4K pages 
    pt[page_index] = (phys_addr & VMM_ADDR_MASK )| VMM_PRESENT | flags;
    vmm_flush_tlb(virt_addr);// flush TLB to update mapping
    return 0;
}

void vmm_unmap(uint64_t virt_addr){
    uint64_t *pdpt = (uint64_t *)KP2V(kernel_pml4[pml4_index(virt_addr)] & VMM_ADDR_MASK);
    if (!(kernel_pml4[pml4_index(virt_addr)] & VMM_PRESENT)) {
        return;
    }

    uint64_t *pd = (uint64_t *)KP2V(pdpt[pdpt_index(virt_addr)] & VMM_ADDR_MASK);
    if (!(pdpt[pdpt_index(virt_addr)] & VMM_PRESENT)) {
        return;
    }

    uint64_t *pt = (uint64_t *)KP2V(pd[pd_index(virt_addr)] & VMM_ADDR_MASK);
    if (!(pd[pd_index(virt_addr)] & VMM_PRESENT)) {
        return;
    }

    uint64_t page_index = pt_index(virt_addr); // represents 12 bits for 4K pages
    pt[page_index] = 0; // clear the page table entry
    vmm_flush_tlb(virt_addr); // flush TLB to update mapping
}

uint64_t vmm_get_phys(uint64_t virt_addr) {
    if (!(kernel_pml4[pml4_index(virt_addr)] & VMM_PRESENT)) {
        return 0;
    }
    uint64_t *pdpt = (uint64_t *)KP2V(kernel_pml4[pml4_index(virt_addr)] & VMM_ADDR_MASK);

  
    if (!(pdpt[pdpt_index(virt_addr)] & VMM_PRESENT)) {
        return 0;
    }
    uint64_t *pd = (uint64_t *)KP2V(pdpt[pdpt_index(virt_addr)] & VMM_ADDR_MASK);

    if (!(pd[pd_index(virt_addr)] & VMM_PRESENT)) {
        return 0;
    }
    uint64_t *pt = (uint64_t *)KP2V(pd[pd_index(virt_addr)] & VMM_ADDR_MASK);

    if (!(pt[pt_index(virt_addr)] & VMM_PRESENT)) {
        return 0;
    }

    return (pt[pt_index(virt_addr)] & VMM_ADDR_MASK) | (virt_addr & 0xfff);
}



void vmm_flush_tlb(uint64_t virt_addr) {
    __asm__ volatile("invlpg (%0)" : : "r" (virt_addr) : "memory");
}

uint64_t vmm_get_kernel_pml4(void) {
    return KV2P(kernel_pml4);
}


// Initialize the virtual memory manager
void vmm_init(void) {
    serial_write_string("\n=== VMM init ===\n");


    // step 1:Allocate a new page for the kernel PML4
    kernel_pml4 = alloc_table();
    if (!kernel_pml4) {
        serial_write_string("FATAL: Could not allocate PML4\n");
        while (1) {}
    }

    serial_write_string("PML4 Allocated at phys: ");
    serial_write_hex(KV2P(kernel_pml4));
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
        
        uint64_t *pdpt = get_or_create_table(kernel_pml4, pml4_index(virt),  VMM_WRITABLE);
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
    // Map physical 0 to 2 MB (kernel + bitmap + stack live here)
    // we keep 4KB granularity for the kernel area for future flexibility
    serial_write_string("Mapping Kernel at 0xFFFFFFFF80000000...\n");
    for (uint64_t phys = 0; phys < 0x200000; phys +=PAGE_SIZE) {
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
    uint64_t new_cr3 = KV2P(kernel_pml4);
    __asm__ volatile("mov %0, %%cr3" : : "r" (new_cr3): "memory");
    serial_write_string("CR3 set to ");
    serial_write_hex(new_cr3);
    serial_write_string("\n");
}