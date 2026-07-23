#ifndef _PMM_H
#define _PMM_H


#include <stdint.h>


// E820 entry as written in stage 2 of the bootloader

struct e820_entry {
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t acpi_attr;
} __attribute__((packed)) ;


// Memory map types from Bios

#define E820_USABLE       1
#define E820_RESERVED     2
#define E820_ACPI_RECLAIM 3
#define E820_ACPI_NVS     4
#define E820_BAD_MEMORY   5


// Virtual memory mapping (Higher half conversion)
#define KERNEL_VIRTUAL_BASE 0xFFFFFFFF80000000ULL // Base address of kernel virtual memory
#define DIRECT_MAP_BASE     0xFFFF800000000000ULL // Base address of direct mapped memory
#define MMIO_BASE           0xFFFFC00000000000ULL // Base address of memory-mapped I/O


// Direct map conversion(for physical addresses in the direct mapped range)
#define V2P(addr)((uint64_t)(addr) - DIRECT_MAP_BASE) // Convert virtual address to physical address
#define P2V(addr)((uint64_t)(addr) + DIRECT_MAP_BASE) // Convert physical address to virtual address

// Kernel range conversions(for kernel symbols like kernel_end, )
#define KV2P(addr)((uint64_t)(addr) - KERNEL_VIRTUAL_BASE) 
#define KP2V(addr)((uint64_t)(addr) + KERNEL_VIRTUAL_BASE) 

// Memory map BUFFER

#define E820_COUNT_ADDR   0X7000
#define E820_ENTRIES_ADDR 0X7004

#define PAGE_SIZE 4096


void pmm_init(void);
void pmm_print_map(void);
void pmm_print_stats(void);

// Physical end (exclusive, page-aligned) of the kernel image + PMM bitmap.
uint64_t pmm_reserved_phys_end(void);


// Returns Physical address of the given virtual address (or NULL if not found)
uint64_t pmm_alloc_page(void);

/* Allocator scan cost: total bit/byte tests performed across all allocations,
 * and how many allocations that was. bits/allocs is the number that matters --
 * it says whether the free-page search is O(1) amortised or walking the bitmap.
 * See the rotating hint in pmm_alloc_page. */
void pmm_scan_stats(uint64_t *out_bits, uint64_t *out_allocs);

// takes a physical address and frees the corresponding virtual page
void pmm_free_page(uint64_t phys_addr);

// Explicitly mark one specific page as used, without allocating it via the
// normal first-fit scan. For physical addresses that something OUTSIDE the
// allocator's own bookkeeping needs to own at a fixed location (e.g. the
// AP trampoline, which must sit at a page-aligned, known address the SMP
// bring-up code pokes machine code into directly) rather than wherever
// pmm_alloc_page() happens to return. Idempotent: reserving an
// already-reserved page is a harmless no-op.
void pmm_reserve_page(uint64_t phys_addr);




#endif // _PMM_H