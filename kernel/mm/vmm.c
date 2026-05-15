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
    uint64_t *table = (uint64_t *)P2V(pyhs_addr);
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
        return (uint64_t *)P2V(phys_addr);
    }
    // Allocate a new table and set its flags
    uint64_t phys_addr = pmm_alloc_page();
    if (!phys_addr) {
        return 0;
    }
    
    uint64_t *table = (uint64_t *)P2V(phys_addr);
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
    uint64_t *pdpt = (uint64_t *)P2V(kernel_pml4[pml4_index(virt_addr)] & VMM_ADDR_MASK);
    if (!(kernel_pml4[pml4_index(virt_addr)] & VMM_PRESENT)) {
        return;
    }

    uint64_t *pd = (uint64_t *)P2V(pdpt[pdpt_index(virt_addr)] & VMM_ADDR_MASK);
    if (!(pdpt[pdpt_index(virt_addr)] & VMM_PRESENT)) {
        return;
    }

    uint64_t *pt = (uint64_t *)P2V(pd[pd_index(virt_addr)] & VMM_ADDR_MASK);
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
    uint64_t *pdpt = (uint64_t *)P2V(kernel_pml4[pml4_index(virt_addr)] & VMM_ADDR_MASK);

  
    if (!(pdpt[pdpt_index(virt_addr)] & VMM_PRESENT)) {
        return 0;
    }
    uint64_t *pd = (uint64_t *)P2V(pdpt[pdpt_index(virt_addr)] & VMM_ADDR_MASK);

    if (!(pd[pd_index(virt_addr)] & VMM_PRESENT)) {
        return 0;
    }
    uint64_t *pt = (uint64_t *)P2V(pd[pd_index(virt_addr)] & VMM_ADDR_MASK);

    if (!(pt[pt_index(virt_addr)] & VMM_PRESENT)) {
        return 0;
    }

    return (pt[pt_index(virt_addr)] & VMM_ADDR_MASK) | (virt_addr & 0xfff);
}



void vmm_flush_tlb(uint64_t virt_addr) {
    __asm__ volatile("invlpg (%0)" : : "r" (virt_addr) : "memory");
}

uint64_t vmm_get_kernel_pml4(void) {
    return V2P(kernel_pml4);
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
    serial_write_hex(V2P(kernel_pml4));
    serial_write_string("\n");


    // step 2: Map 0 to 1GB physical memory to higher half virtual memory
    // virt 0xFFFFFFFF80000000 -> phys 0x0
    // virt 0xFFFFFFFF80100000 -> phys 0x100000
    // ... etc 1GB
    serial_write_string("Mapping 1GB to higher half...\n");
    for (uint64_t addr = 0; addr < 0x40000000; addr += PAGE_SIZE) {
        uint64_t virt_addr = P2V(addr);
        if (vmm_map(virt_addr, addr, VMM_WRITABLE) < 0) {
            serial_write_string("FATAL: vmm_map failed at\n");
            serial_write_hex(addr);
            serial_write_string("\n");
            while (1) {}
        }
    }
       
    
    serial_write_string("Mapping complete\n");

    // step 3: switch CR3 to point to the kernel PML4 (new page tables)

    serial_write_string("switching CR3 ...\n");
    uint64_t new_cr3 = V2P(kernel_pml4);
    __asm__ volatile("mov %0, %%cr3" : : "r" (new_cr3));
    serial_write_string("CR3 set to ");
    serial_write_hex(new_cr3);
    serial_write_string("\n");
}