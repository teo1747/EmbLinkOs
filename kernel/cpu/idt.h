#ifndef _IDT_H
#define _IDT_H
#include <stdint.h>


// IDT (Interrupt Descriptor Table) definitions and declarations


// IDT entry structure (8 bytes)
struct idt_entry {
    uint16_t offset_low;   // Lower 16 bits of handler function address
    uint16_t selector;     // Code segment selector in GDT(0x08)
    uint8_t  ist;          // Interrupt Stack Table index 2-bits, reserved 5-bits
    uint8_t  type_attr;    // P, DPL, Type
    uint16_t offset_mid;   // Middle 16 bits of handler function address
    uint32_t offset_high;  // Higher 32 bits of handler function address
    uint32_t reserved;     //  set to 0
} __attribute__((packed)); // Packed to prevent compiler padding


// IDT pointer structure (6 bytes) for lidt instruction
struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));


void idt_init(void);
void idt_set_entry(uint8_t vector, uint64_t handler, uint8_t type_attr);

// Like idt_set_entry, but selects an Interrupt Stack Table slot (1..7) so the
// CPU switches to TSS.ist[ist-1] on entry instead of using the current stack.
// ist == 0 means "no IST" (use the regular stack), identical to idt_set_entry.
// Used for #DF (vector 8): a corrupted/overflowed kernel stack must not be
// reused by the handler, or the fault escalates to a triple-fault reset.
void idt_set_entry_ist(uint8_t vector, uint64_t handler, uint8_t type_attr, uint8_t ist);


#endif // _IDT_H