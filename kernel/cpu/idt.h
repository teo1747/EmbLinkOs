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


#endif // _IDT_H