#include "vmm.h"
#include "pmm.h"
#include "../drivers/serial.h"
#include <stdint.h>


static uint64_t kernel_pml4_phys = 0;     // physical address of the kernel PML4
static int vmm_direct_map_active = 0;     // 0 during early init, 1 after the CR3 switch
extern uint8_t kernel_end[];
extern uint8_t BOOT_STACK_TOP_PHYS[];


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


// Public API
int vmm_map(uint64_t virt_addr, uint64_t phys_addr, uint64_t flags) {
    uint64_t *pdpt = get_or_create_table(vmm_pml4(), pml4_index(virt_addr),  flags);
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
    uint64_t *pml4 = vmm_pml4();
    if (!(pml4[pml4_index(virt_addr)] & VMM_PRESENT)) {
        return;
    }
    uint64_t *pdpt = (uint64_t *)vmm_table(pml4[pml4_index(virt_addr)] & VMM_ADDR_MASK);

    if (!(pdpt[pdpt_index(virt_addr)] & VMM_PRESENT)) {
        return;
    }
    uint64_t *pd = (uint64_t *)vmm_table(pdpt[pdpt_index(virt_addr)] & VMM_ADDR_MASK);

    if (!(pd[pd_index(virt_addr)] & VMM_PRESENT)) {
        return;
    }
    uint64_t *pt = (uint64_t *)vmm_table(pd[pd_index(virt_addr)] & VMM_ADDR_MASK);

    uint64_t page_index = pt_index(virt_addr); // represents 12 bits for 4K pages
    pt[page_index] = 0; // clear the page table entry
    vmm_flush_tlb(virt_addr); // flush TLB to update mapping
}

uint64_t vmm_get_phys(uint64_t virt_addr) {
    uint64_t *pml4 = vmm_pml4();
    if (!(pml4[pml4_index(virt_addr)] & VMM_PRESENT)) {
        return 0;
    }
    uint64_t *pdpt = (uint64_t *)vmm_table(pml4[pml4_index(virt_addr)] & VMM_ADDR_MASK);


    if (!(pdpt[pdpt_index(virt_addr)] & VMM_PRESENT)) {
        return 0;
    }
    uint64_t *pd = (uint64_t *)vmm_table(pdpt[pdpt_index(virt_addr)] & VMM_ADDR_MASK);

    if (!(pd[pd_index(virt_addr)] & VMM_PRESENT)) {
        return 0;
    }
    uint64_t *pt = (uint64_t *)vmm_table(pd[pd_index(virt_addr)] & VMM_ADDR_MASK);

    if (!(pt[pt_index(virt_addr)] & VMM_PRESENT)) {
        return 0;
    }

    return (pt[pt_index(virt_addr)] & VMM_ADDR_MASK) | (virt_addr & 0xfff);
}



void vmm_flush_tlb(uint64_t virt_addr) {
    __asm__ volatile("invlpg (%0)" : : "r" (virt_addr) : "memory");
}

uint64_t vmm_get_kernel_pml4(void) {
    return kernel_pml4_phys;
}


// Initialize the virtual memory manager
void vmm_init(void) {
    serial_write_string("\n=== VMM init ===\n");


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
    // Map [0 .. round_up(KV2P(kernel_end), 4K)) so the kernel can grow past 2 MB
    // without faulting after CR3 switch. Keep a floor tied to boot stack top
    // exported by the linker (stage2 currently sets rsp to higher-half + this
    // physical value; stack grows down, so pages below this boundary are needed).
    uint64_t kernel_end_phys = KV2P(kernel_end);
    uint64_t boot_stack_floor = (uint64_t)BOOT_STACK_TOP_PHYS;
    uint64_t kernel_map_end = (kernel_end_phys + (PAGE_SIZE - 1)) & ~(PAGE_SIZE - 1);
    if (kernel_map_end < boot_stack_floor)
        kernel_map_end = boot_stack_floor;

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
    uint64_t virt_base = mmio_next_virt;
    mmio_next_virt += total_size;

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