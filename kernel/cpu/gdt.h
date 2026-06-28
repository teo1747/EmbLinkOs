#ifndef __GDT__H
#define __GDT__H

#include <stdint.h>


#define GDT_ENTRIES 7      // Number of GDT entries: null, kernel code, kernel data, user code, user data, TSS (2 entries)

// GDT entry structure (8 bytes)
struct gdt_entry {
    uint16_t limit_low;      // Lower 16 bits of segment limit
    uint16_t base_low;      // Lower 16 bits of segment base address
    uint8_t base_mid;       // Next 8 bits of segment base address
    uint8_t access;         // Access flags
    uint8_t granularity;    // Granularity and upper 4 bits of segment limit
    uint8_t base_high;      // Last 8 bits of segment base address
} __attribute__((packed));


/* The 16-bytes TSS descriptor: an 8-byte gdt_entry shape plus the upper 8 bytes for the base address and 
 * a reserved dword. It spans two GDT entries. */
struct tss_descriptor {
    struct gdt_entry entry;  // First 8 bytes: GDT entry
    uint32_t base_upper;     // Next 4 bytes: upper 32 bits of TSS base address
    uint32_t reserved;       // Last 4 bytes: reserved, must be zero
} __attribute__((packed));


// GDT pointer structure (6 bytes)
struct gdt_ptr {
    uint16_t limit;         // Size of GDT - 1
    uint64_t base;          // Address of the first GDT entry
} __attribute__((packed));


/* The 16bit TSS (SDM Vol. 3A, (FIG 7.11) 7.11). Hardware Task switching is gone in
 * in long mode. this exists mainly to hold RSP0 (THE KERNEL stack the cpu loads
 * on a privilege level change) and the IST entries. IOmaps_base set past the limit
 * = no I/O permission bitmap */
struct tss {
    uint32_t reserved0;
    uint64_t rsp0;          // Stack pointer for ring 0
    uint64_t rsp1;          // Stack pointer for ring 1 (not used)
    uint64_t rsp2;          // Stack pointer for ring 2 (not used)
    uint64_t reserved1;
    uint64_t ist1;          // Interrupt Stack Table entry 1
    uint64_t ist2;          // Interrupt Stack Table entry 2
    uint64_t ist3;          // Interrupt Stack Table entry 3
    uint64_t ist4;          // Interrupt Stack Table entry 4
    uint64_t ist5;          // Interrupt Stack Table entry 5
    uint64_t ist6;          // Interrupt Stack Table entry 6
    uint64_t ist7;          // Interrupt Stack Table entry 7
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;     // Offset to the I/O permission bitmap (set past the limit to disable)
} __attribute__((packed));



// Build the kernel GDT and load it
void gdt_init(void);
void tss_set_rsp0(uint64_t rsp0);  // Set the RSP0 field in the TSS (kernel stack pointer for ring 0)



#endif /* __GDT__H */