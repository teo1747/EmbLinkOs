#include "arch/x86_64/cpu/gdt.h"
#include "arch/x86_64/cpu/percpu.h"
#include "include/kprintf.h"
#include "drivers/char/serial.h"




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


/* Write the 16-byte TSS descriptor across gdt[index] and gdt[index + 1].
 * Unlike code/data, the base MATTTERS here - it points the CPU at g_tss.*/
static void set_tss_descriptor(int index, struct tss *tss) {
    uint64_t base = (uint64_t)tss;
    gdt[index].limit_low = sizeof(struct tss) - 1;
    gdt[index].base_low = (uint16_t)(base & 0xFFFF);
    gdt[index].base_mid = (uint8_t)((base >> 16) & 0xFF);
    gdt[index].access = 0x89; // Present, ring 0, type=9 (available 64-bit TSS)
    gdt[index].granularity = 0x00; // Flags: 0 for TSS
    gdt[index].base_high = (uint8_t)((base >> 24) & 0xFF);

    // Upper 32 bits of the base address go into the next GDT entry
    struct tss_descriptor *tss_desc = (struct tss_descriptor *)&gdt[index];
    tss_desc->base_upper = (uint32_t)(base >> 32);
    tss_desc->reserved = 0;
}


void tss_set_rsp0(uint64_t rsp0) {
    this_cpu()->tss.rsp0 = rsp0;
}

/* Shared by gdt_init_bsp() and gdt_init_this_cpu(): everything that must
 * happen on EVERY core, not just once for the whole (shared, in-memory)
 * table -- GDTR/segment-register/task-register are per-core CPU state,
 * even though the table they all point into is one shared region of
 * memory. `cpu_index` selects which of the MAX_CPUS reserved TSS
 * descriptor slots (index 5 onward, 2 GDT entries each) and which
 * `cpu_table[cpu_index].tss`/`rsp0_stack`/`df_stack` this core uses. */
static void gdt_load_and_set_tss(uint32_t cpu_index) {
    struct cpu_data *me = &cpu_table[cpu_index];

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

    /* TSS: stacks grow DOWN, so RSP starts at the TOP of each buffer. */
    me->tss.rsp0 = (uint64_t)(me->rsp0_stack + sizeof(me->rsp0_stack));
    me->tss.ist1 = (uint64_t)(me->df_stack + sizeof(me->df_stack));
    me->tss.iomap_base = sizeof(struct tss); // No I/O permission bitmap

    /* Slot 5 + 2*cpu_index / 5 + 2*cpu_index + 1: this core's own 16-byte
     * TSS descriptor, distinct from every other core's. */
    int tss_slot = 5 + 2 * (int)cpu_index;
    set_tss_descriptor(tss_slot, &me->tss);

    /* Load the Task Register with THIS core's own TSS selector
     * (tss_slot << 3) -- e.g. cpu_index 0 -> 0x28, cpu_index 1 -> 0x38,
     * matching the existing single-core selector (0x28) exactly for the
     * BSP, so no other code (IDT setup, #DF IST reference, etc.) needs to
     * change. */
    uint16_t tss_selector = (uint16_t)(tss_slot << 3);
    __asm__ volatile ("ltr %0" : : "r"(tss_selector) : "memory");
}

void gdt_init_bsp(void) {
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

    // Entry 3: user data segment (selector 0x18). Access byte 0xF2 = 11110010b (present, ring 3, data segment, writable), Flags 0x00 (not a code segment)
    set_gdt_entry(3, 0xF2, 0x00);

    // Entry 4: User code segment (selector 0x20). Access byte 0xFA = 11111010b (present, ring 3, code segment, executable, readable), Flags 0x20 = 00100000b (long mode)
    set_gdt_entry(4, 0xFA, 0x20);

    // Set up the GDT pointer -- sized for the WHOLE table (all MAX_CPUS TSS
    // slots), even though only cpu_table[0]'s slot is populated yet. Every
    // other slot's descriptor bytes are all-zero (not-present) until that
    // core's own gdt_init_this_cpu() call fills it in -- harmless, since
    // nothing references those selectors before then.
    gp_ptr.limit = sizeof(gdt) - 1;
    gp_ptr.base = (uint64_t)&gdt;

    /* cpu_table[0] is always the BSP by definition (see percpu.h) -- this
     * runs before acpi_init()/lapic_init(), so this_cpu() isn't usable yet
     * and cpu_index 0 is used directly rather than looked up. */
    gdt_load_and_set_tss(0);

    kprintf("GDT + TSS loaded at virtual address: ");
    kprintf("%p", &gdt);
    kprintf("\nSegment reloaded\n");
    uint16_t tr;
    __asm__ volatile ("str %0" : "=r"(tr));   /* read the Task Register back */
    serial_write_string("TR="); serial_write_hex(tr); serial_write_string("\n");
}

void gdt_init_this_cpu(void) {
    /* Unlike gdt_init_bsp(), this runs on an AP well after the BSP has
     * completed acpi_init()/lapic_init()/percpu_init_topology() -- so
     * this_cpu() correctly resolves to THIS core's own cpu_table[] entry
     * via its LAPIC ID, without needing to be told its index explicitly. */
    struct cpu_data *me = this_cpu();
    gdt_load_and_set_tss(me->cpu_index);
}
