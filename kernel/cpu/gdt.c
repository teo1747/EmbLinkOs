#include "gdt.h"
#include "../drivers/serial.h"

#define GDT_ENTRIES 3

static struct gdt_entry gdt[GDT_ENTRIES];
static struct gdt_ptr gp_ptr;


// Econde one GDT entry and set it in the GDT table at the given index
static void set_gdt_entry(int index, uint8_t access, uint8_t flags) {
    // In long mode, base and limit are ignored for CS/DS/ES/SS.
    // We zero them out for simplicity. The access and flags bytes are still used to set the segment type and attributes.
    gdt[index].limit_low = 0;
    gdt[index].base_low = 0;
    gdt[index].base_mid = 0;
    gdt[index].access = access;
    gdt[index].granularity = flags;
    gdt[index].base_high = 0;
}


void gdt_init(void) {
    serial_write_string("\n=== GDT init ===\n");

    // Null descriptor
    set_gdt_entry(0, 0, 0);

    // Entry 1: Kernel code segment (selector 0x08). Access byte 0x9A = 10011010b (present, ring 0, code segment, executable, readable), Flags 0x20 = 00100000b (long mode)
    // Access = 0x9A = 1001 10101
    // bit 7 (present) = 1 : Present
    // bit 6-5 (DPL) = 00 : Ring 0
    // bit 4 (S) = 1 : Code/Data segment
    // bit 3-0 (type) = 1010 : Code segment, executable, readable
    // Flags = 0x20 = 0010 0000
    // bit 7 (granularity) = 0 : Byte granularity (ignored in long mode)
    // bit 6 (size) = 0 : 16-bit protected mode (ignored in long mode)
    // bit 5 (long) = 1 : 64-bit code segment
    // bit 4 (AVL) = 0 : Available for system use
    // bit 3-0 (limit high) = 0000 : Upper 4

    set_gdt_entry(1, 0x9A, 0x20);

    // Entry 2: Kernel data segment (selector 0x10). Access byte 0x92 = 10010010b (present, ring 0, data segment, writable), Flags 0x00 (not a code segment)
    // Access = 0x92 = 1001 0010b
    // bit 7 (present) = 1 : Present
    // bit 6-5 (DPL) = 00 : Ring 0
    // bit 4 (S) = 1 : Code/Data segment
    // bit 3-0 (type) = 0010 : Data segment, writable
    // Flags = 0x00 = 0000 0000b
    set_gdt_entry(2, 0x92, 0x00);

    // Set up the GDT pointer
    gp_ptr.limit = sizeof(gdt) - 1;
    gp_ptr.base = (uint64_t)&gdt;


    // Load the GDT using lgdt instruction in assembly
    __asm__ volatile ("lgdt %0" : : "m"(gp_ptr));

    // After loading the new GDT, we need to update the segment registers to use the new code and data segments. This is typically done with a far jump to flush the instruction pipeline and then reloading the data segment registers.
    // We need to reload them so the cpu te-reads from the new GDT.
    // CS can't be loaded directly, we have to do a far jump to the new code segment selector (0x08) to update CS. Then we can load the data segment registers (DS, ES, FS, GS, SS) with the new data segment selector (0x10).
    __asm__ volatile (
        "pushq $0x08\n"      // Push new code segment selector CS
        "leaq 1f(%%rip), %%rax\n" // Load address of label 1 into RAX   
        "pushq %%rax\n"      // Push address of label 1
        "lretq\n"            // Far return to update CS
        "1:\n"               // Label 1: execution continues here after far jump
        "mov $0x10, %%ax\n" // Load new data segment selector into AX
        "mov %%ax, %%ds\n"  // Update DS
        "mov %%ax, %%es\n"  // Update ES
        "mov %%ax, %%fs\n"  // Update FS
        "mov %%ax, %%gs\n"  // Update GS
        "mov %%ax, %%ss\n"  // Update SS
        : : : "rax", "memory"
    );

    serial_write_string("GDT loaded at virtual address: ");
    serial_write_hex((uint64_t)&gdt);
    serial_write_string("\nSegment reloaded\n");
}