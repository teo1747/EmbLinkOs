#ifndef __GDT__H
#define __GDT__H

#include <stdint.h>


// GDT entry structure (8 bytes)
struct gdt_entry {
    uint16_t limit_low;      // Lower 16 bits of segment limit
    uint16_t base_low;      // Lower 16 bits of segment base address
    uint8_t base_mid;       // Next 8 bits of segment base address
    uint8_t access;         // Access flags
    uint8_t granularity;    // Granularity and upper 4 bits of segment limit
    uint8_t base_high;      // Last 8 bits of segment base address
} __attribute__((packed));


// GDT pointer structure (6 bytes)
struct gdt_ptr {
    uint16_t limit;         // Size of GDT - 1
    uint64_t base;          // Address of the first GDT entry
} __attribute__((packed));

// Build the kernel GDT and load it
void gdt_init(void);



#endif /* __GDT__H */