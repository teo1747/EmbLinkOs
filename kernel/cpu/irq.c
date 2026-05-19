#include "irq.h"
#include "pic.h"
#include "idt.h"
#include "../drivers/serial.h"

#include <stdint.h>


// Match IRQ number to handler function pointer/ isr.asm

struct registers {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, error_code; // pushed by isr.asm
    uint64_t rip, cs, rflags, rsp, ss; // pushed by CPU automatically
};


// External assembly stubs
extern void irq0(void);        extern void irq1(void);
extern void irq2(void);        extern void irq3(void);
extern void irq4(void);        extern void irq5(void);
extern void irq6(void);        extern void irq7(void);
extern void irq8(void);        extern void irq9(void);
extern void irq10(void);       extern void irq11(void);
extern void irq12(void);       extern void irq13(void);
extern void irq14(void);       extern void irq15(void);


// Table of registered IRQ handlers. Indexed by IRQ number (0-15)
static irq_handler_t irq_handlers[16] = {0};


void irq_install(void) {
    idt_set_entry(32, (uint64_t)irq0, 0x8E);
    idt_set_entry(33, (uint64_t)irq1, 0x8E);
    idt_set_entry(34, (uint64_t)irq2, 0x8E);
    idt_set_entry(35, (uint64_t)irq3, 0x8E);
    idt_set_entry(36, (uint64_t)irq4, 0x8E);
    idt_set_entry(37, (uint64_t)irq5, 0x8E);
    idt_set_entry(38, (uint64_t)irq6, 0x8E);
    idt_set_entry(39, (uint64_t)irq7, 0x8E);
    idt_set_entry(40, (uint64_t)irq8, 0x8E);
    idt_set_entry(41, (uint64_t)irq9, 0x8E);
    idt_set_entry(42, (uint64_t)irq10, 0x8E);
    idt_set_entry(43, (uint64_t)irq11, 0x8E);
    idt_set_entry(44, (uint64_t)irq12, 0x8E);
    idt_set_entry(45, (uint64_t)irq13, 0x8E);
    idt_set_entry(46, (uint64_t)irq14, 0x8E);
    idt_set_entry(47, (uint64_t)irq15, 0x8E);

    serial_write_string("\n=== IRQ install ===\n");
    serial_write_string("IRQ stubs installed ( IDT 32-47)\n");

}


void irq_register(uint8_t irq, irq_handler_t handler) {
    if (irq >= 16) return; // Invalid IRQ number
        irq_handlers[irq] = handler;
        pic_unmask_irq(irq); // Unmask the IRQ at the PIC
    
}


void irq_unregister(uint8_t irq) {
    if (irq >= 16) return; // Invalid IRQ number
    irq_handlers[irq] = 0;
    pic_mask_irq(irq); // Mask the IRQ at the PIC
}


// called from assembly stub with IRQ number in rdi
void irq_handler(struct registers *regs) {
    uint8_t irq = (uint8_t)(regs->vector - 32); // IRQ number is vector - 32
    // DEBUG: signal that we entered
    if (irq_handlers[irq]) {
        irq_handlers[irq](); // Call the registered handler
    }else {
        serial_write_string("[Unhandled IRQ: ");
        serial_write_hex(irq);
        serial_write_string("]\n");
    }

    pic_send_eoi(irq); // Send End of Interrupt to PIC
}


