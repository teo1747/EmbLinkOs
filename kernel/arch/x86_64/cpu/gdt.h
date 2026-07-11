#ifndef __GDT__H
#define __GDT__H

#include <stdint.h>

/* Generous capacity for per-core metadata (TSS slots below, and
 * kernel/cpu/percpu.h's struct cpu_data table) -- defined here rather than
 * in percpu.h specifically to avoid a circular include (percpu.h needs
 * `struct tss` from this header; this header's GDT_ENTRIES needs a CPU
 * count). Distinct from ACPI_MAX_CPUS (256, acpi.h), which is MADT-parsing
 * capacity, a different concern. */
#define MAX_CPUS 32

/* 5 shared entries (null, kernel code, kernel data, user data, user code)
 * plus one 16-byte (2-GDT-entry) TSS descriptor PER CORE -- was a single
 * TSS for the whole kernel; every core now needs its own (each takes
 * ring-3->ring-0 interrupts on its own kernel stack, so TSS.RSP0 can't be
 * shared). See gdt_init_this_cpu()'s comment for the selector math. */
#define GDT_ENTRIES (5 + 2 * MAX_CPUS)

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



/* Build the shared part of the GDT (the 5 code/data descriptors) and load
 * it for the BSP -- called exactly once, very early in kernel_main(),
 * BEFORE acpi_init()/lapic_init() (so this_cpu() isn't usable yet; this
 * function operates on cpu_table[0] directly, which is always the BSP by
 * definition). Internally calls gdt_load_and_set_tss(0) to finish setting
 * up the BSP's own TSS/segment registers/task register -- see that
 * function's comment for why AP bring-up (Phase 2) doesn't duplicate this
 * logic. */
void gdt_init_bsp(void);

/* Called by an AP (Phase 2's ap_main()) once it has a working C
 * environment: loads the (already-built, shared) GDT, sets up THIS core's
 * own TSS descriptor slot and RSP0/IST1 stacks, reloads segment registers,
 * and does `ltr` with this core's own TSS selector. Requires this_cpu() to
 * already resolve correctly (i.e. percpu_init_topology() must have already
 * run on the BSP) -- true by the time any AP reaches this call, since APs
 * only start after the BSP has gotten through ACPI/LAPIC init. */
void gdt_init_this_cpu(void);

void tss_set_rsp0(uint64_t rsp0);  // Set the RSP0 field in THIS core's own TSS (kernel stack pointer for ring 0)



#endif /* __GDT__H */