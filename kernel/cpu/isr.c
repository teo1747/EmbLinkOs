#include <stdint.h>
#include "../drivers/serial.h"


// structure to hold the CPU register state during an interrupt
struct registers {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, error_code; // Interrupt vector number and error code (if applicable)
    uint64_t rip, cs, rflags, rsp, ss;
} __attribute__((packed));


static const char *exception_messages[] = {
    "Division By Zero",
    "Debug",
    "Non Maskable Interrupt",
    "Breakpoint",
    "Overflow",
    "Bound Range Exceeded",
    "Invalid Opcode",
    "Device Not Available",
    "Double Fault",
    "Coprocessor Segment Overrun",
    "Invalid TSS",
    "Segment Not Present",
    "Stack-Segment Fault",
    "General Protection Fault",
    "Page Fault",
    "Reserved (15)",
    "x87 Floating-Point Exception",
    "Alignment Check",
    "Machine Check",
    "SIMD Floating-Point Exception",
    "Virtualization Exception",
    "Control Protection Exception",
    "Reserved (22)",
    "Reserved (23)",
    "Reserved (24)",
    "Reserved (25)",
    "Reserved (26)",
    "Reserved (27)",
    "Reserved (28)",
    "Reserved (29)",
    "Hypervisor Injection Exception",
    "VMM Communication Exception",
};


void isr_handler(struct registers *regs) {
    
    serial_write_string("\n=== Exception ===\n");
    serial_write_string("Vector: ");
    serial_write_hex(regs->vector);
    serial_write_string(" (");

    if (regs->vector < 32) {
        serial_write_string(exception_messages[regs->vector]);
    } else {
        serial_write_string("Unknown Exception");
        }

    serial_write_string(")\n");

    serial_write_string("Error code: ");
    serial_write_hex(regs->error_code);
    serial_write_string("\n");

    serial_write_string("RIP: ");
    serial_write_hex(regs->rip);
    serial_write_string("\n");

    serial_write_string("RSP: ");
    serial_write_hex(regs->rsp);
    serial_write_string("\n");

    serial_write_string("RBP: ");
    serial_write_hex(regs->rbp);
    serial_write_string("\n");

    serial_write_string("system halted.\n");

    // In a real kernel, you would likely halt the system or attempt recovery here
    while (1) {
            __asm__ volatile ("cli; hlt");
        }
    
}