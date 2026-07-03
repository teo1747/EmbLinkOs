#ifndef _VMM_H_
#define _VMM_H_
#include <stdint.h>


// Page table entry flags
#define VMM_PRESENT       (1ULL << 0)
#define VMM_WRITABLE      (1ULL << 1)
#define VMM_USER          (1ULL << 2)
#define VMM_WRITETHROUGH  (1ULL << 3)
#define VMM_NOCACHE       (1ULL << 4)
#define VMM_ACCESSED      (1ULL << 5)
#define VMM_DIRTY         (1ULL << 6)
#define VMM_HUGE          (1ULL << 7)
#define VMM_GLOBAL        (1ULL << 8)
#define VMM_NX            (1ULL << 63)

// Address mask for physical addresses in page table entries
#define VMM_ADDR_MASK 0x0000fffffffff000ULL

// Initialize VMM and switch to kernel paage tables
void vmm_init(void);

// Map a virtual address to a physical address with the given flags
// Return 0 on succes, -1 on failure
int vmm_map(uint64_t virt, uint64_t phys, uint64_t flags);

// Unmap a virtual address
void vmm_unmap(uint64_t virt);

// Get the physical address of a virtual address (returns 0 if not mapped)
uint64_t vmm_get_phys(uint64_t virt);

// Invalidates the TLB entry for the given virtual address
void vmm_flush_tlb(uint64_t virt);

// Get the kernel's PML4 (for context switching)
uint64_t vmm_get_kernel_pml4(void);

// Get the current process's PML4 (for context switching)
//uint64_t vmm_get_current_pml4(void);

// Map a contiguous Physical MMIO region to a fresh virtual address
// In the MMIO_BASE range. Returns the virtual address on success, 0 on failure.
// Size in bytes, will be rounded up to a multiple of PAGE_SIZE.
uint64_t vmm_map_mmio(uint64_t phys, uint64_t size);

// Map a virtual address to a physical address in a specific PML4 (address space)
int vmm_map_in(uint64_t pml4_phys, uint64_t virt, uint64_t phys, uint64_t flags);

// Create a new address space (PML4) and return its physical address
uint64_t vmm_create_address_space(void);

// Switch to a new address space by loading CR3 with the given PML4 physical address
void vmm_switch_address_space(uint64_t pml4_phys);

// Look up the physical address a VA maps to IN A GIVEN address space.
// Mirrors vmm_get_phys but walks `pml4_phys` instead of the kernel PML4 — needed so elf_load can find the frame it just mapped into the process space and reach

uint64_t vmm_get_phys_in(uint64_t pml4_phys, uint64_t virt_addr) ;

#endif
