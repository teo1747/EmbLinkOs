#include "idt.h"

#define IDT_ENTRIES 256


static struct idt_entry idt[IDT_ENTRIES];
static struct idt_ptr idt_ptr;


//External assembly stub for loading the IDT
// isr interrupt service routines for CPU exceptions (0-31)

extern void isr0(void);
extern void isr1(void);
extern void isr2(void);
extern void isr3(void);
extern void isr4(void);
extern void isr5(void);
extern void isr6(void);
extern void isr7(void);
extern void isr8(void);
extern void isr9(void);
extern void isr10(void);
extern void isr11(void);
extern void isr12(void);
extern void isr13(void);
extern void isr14(void);
extern void isr15(void);
extern void isr16(void);
extern void isr17(void);
extern void isr18(void);
extern void isr19(void);
extern void isr20(void);
extern void isr21(void);
extern void isr22(void);
extern void isr23(void);
extern void isr24(void);
extern void isr25(void);
extern void isr26(void);
extern void isr27(void);
extern void isr28(void);
extern void isr29(void);
extern void isr30(void);
extern void isr31(void);    


void idt_set_entry_ist(uint8_t vector, uint64_t handler, uint8_t type_attr, uint8_t ist) {
    idt[vector].offset_low = handler & 0xFFFF;
    idt[vector].selector = 0x08; // kernel code segment selector in GDT
    idt[vector].ist = ist & 0x7; // low 3 bits = IST index; bits 3-7 reserved (0)
    idt[vector].type_attr = type_attr;
    idt[vector].offset_mid = (handler >> 16) & 0xFFFF;
    idt[vector].offset_high = (handler >> 32) & 0xFFFFFFFF;
    idt[vector].reserved = 0;
}

void idt_set_entry(uint8_t vector, uint64_t handler, uint8_t type_attr) {
    idt_set_entry_ist(vector, handler, type_attr, 0); // no IST
}


void idt_init(void) {
    // Set up the IDT pointer
    idt_ptr.limit = sizeof(idt) - 1;
    idt_ptr.base = (uint64_t)&idt;

    // Initialize handlers for cpu exceptions (0-31)
    idt_set_entry(0,  (uint64_t)isr0,  0x8E);
    idt_set_entry(1,  (uint64_t)isr1,  0x8E);
    idt_set_entry(2,  (uint64_t)isr2,  0x8E);
    idt_set_entry(3,  (uint64_t)isr3,  0x8E);
    idt_set_entry(4,  (uint64_t)isr4,  0x8E);
    idt_set_entry(5,  (uint64_t)isr5,  0x8E);
    idt_set_entry(6,  (uint64_t)isr6,  0x8E);
    idt_set_entry(7,  (uint64_t)isr7,  0x8E);
    idt_set_entry(8,  (uint64_t)isr8,  0x8E);
    idt_set_entry(9,  (uint64_t)isr9,  0x8E);
    idt_set_entry(10, (uint64_t)isr10, 0x8E);
    idt_set_entry(11, (uint64_t)isr11, 0x8E);
    idt_set_entry(12, (uint64_t)isr12, 0x8E);
    idt_set_entry(13, (uint64_t)isr13, 0x8E);
    idt_set_entry(14, (uint64_t)isr14, 0x8E);
    idt_set_entry(15, (uint64_t)isr15, 0x8E);
    idt_set_entry(16, (uint64_t)isr16, 0x8E);
    idt_set_entry(17, (uint64_t)isr17, 0x8E);
    idt_set_entry(18, (uint64_t)isr18, 0x8E);
    idt_set_entry(19, (uint64_t)isr19, 0x8E);
    idt_set_entry(20, (uint64_t)isr20, 0x8E);
    idt_set_entry(21, (uint64_t)isr21, 0x8E);
    idt_set_entry(22, (uint64_t)isr22, 0x8E);
    idt_set_entry(23, (uint64_t)isr23, 0x8E);
    idt_set_entry(24, (uint64_t)isr24, 0x8E);
    idt_set_entry(25, (uint64_t)isr25, 0x8E);
    idt_set_entry(26, (uint64_t)isr26, 0x8E);
    idt_set_entry(27, (uint64_t)isr27, 0x8E);
    idt_set_entry(28, (uint64_t)isr28, 0x8E);
    idt_set_entry(29, (uint64_t)isr29, 0x8E);
    idt_set_entry(30, (uint64_t)isr30, 0x8E);
    idt_set_entry(31, (uint64_t)isr31, 0x8E);

    // Load the IDT using lidt instruction
    __asm__ volatile ("lidt %0" : : "m"(idt_ptr));
}