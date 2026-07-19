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

// Set EFER.NXE on the CALLING core. Per-core MSR -- every AP must call this
// before touching any VMM_NX-mapped memory (see vmm.c's comment).
void vmm_enable_nx_this_cpu(void);

// Program IA32_PAT on the CALLING core so PAT entry 4 = Write-Combining (what
// vmm_map_mmio_wc uses). Per-core MSR, like vmm_enable_nx_this_cpu -- every AP
// must call it (ap_main) so a WC mapping is interpreted consistently everywhere.
void vmm_pat_init_this_cpu(void);

// Map a virtual address to a physical address with the given flags
// Return 0 on succes, -1 on failure
int vmm_map(uint64_t virt, uint64_t phys, uint64_t flags);

// Unmap a virtual address
void vmm_unmap(uint64_t virt);

// Clear the PTE for `virt` in a SPECIFIC address space WITHOUT freeing the
// backing frame (for detaching shared memory before an address space is torn
// down -- see vmm.c and kernel/gfx/surface.c).
void vmm_unmap_in(uint64_t pml4_phys, uint64_t virt);

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
// vmm_map_mmio maps strictly UNCACHEABLE; vmm_map_mmio_wc maps WRITE-COMBINING
// (linear framebuffers / large write-mostly apertures only -- never registers).
uint64_t vmm_map_mmio(uint64_t phys, uint64_t size);
uint64_t vmm_map_mmio_wc(uint64_t phys, uint64_t size);

// Unmap and release an MMIO range from vmm_map_mmio[_wc]: clears the PTEs and
// returns the VA range to a free list for reuse (first-fit). Pass the VA the
// map call returned and the same size.
void vmm_unmap_mmio(uint64_t virt, uint64_t size);

// Map n (possibly scattered) physical pages into a contiguous, cached kernel VA
// window (a flat kernel view of shared pixel pages). Returns base VA or 0.
uint64_t vmm_kmap_pages(const uint64_t *phys, uint32_t n);
void     vmm_kunmap_pages(uint64_t virt_base, uint32_t n);

// Map a virtual address to a physical address in a specific PML4 (address space)
int vmm_map_in(uint64_t pml4_phys, uint64_t virt, uint64_t phys, uint64_t flags);

// Create a new address space (PML4) and return its physical address
uint64_t vmm_create_address_space(void);

// Destroy a process address space: frees user-half frames, user page-table pages,
// and the PML4 itself, while leaving the shared kernel half untouched.
void vmm_destroy_address_space(uint64_t pml4_phys);

// Switch to a new address space by loading CR3 with the given PML4 physical address
void vmm_switch_address_space(uint64_t pml4_phys);

// Look up the physical address a VA maps to IN A GIVEN address space.
// Mirrors vmm_get_phys but walks `pml4_phys` instead of the kernel PML4 — needed so elf_load can find the frame it just mapped into the process space and reach

uint64_t vmm_get_phys_in(uint64_t pml4_phys, uint64_t virt_addr) ;

// Allocate a kernel-mode stack of `size` bytes (rounded up to a whole number
// of pages) with one unmapped guard page immediately below it, so a stack
// overflow faults immediately instead of silently corrupting whatever memory
// happens to sit below it (as a plain kmalloc'd stack would). Returns the
// virtual address of the TOP of the stack (the seed value for RSP — x86
// stacks grow down), or 0 on failure. VA space is bump-allocated and not
// reclaimed (same simplification as vmm_map_mmio); only the backing physical
// pages are freed, by vmm_free_kernel_stack.
uint64_t vmm_alloc_kernel_stack(uint64_t size);

// Free the physical pages backing a stack returned by vmm_alloc_kernel_stack.
// `size` must be the same value passed to the matching alloc call.
void vmm_free_kernel_stack(uint64_t stack_top, uint64_t size);

#endif
