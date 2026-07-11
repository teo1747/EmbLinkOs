#include <stdint.h>
#include "drivers/char/serial.h"
#include "arch/x86_64/cpu/spinlock.h"

/* Guards the exception dump below. Deliberately never unlocked: if a
 * second core also faults while this one is dumping/halted, it should
 * just wait here forever rather than interleave its own dump into this
 * one byte-by-byte (serial_write_* has no locking of its own -- observed
 * directly as two simultaneous double faults on different cores producing
 * an unreadable interleaved crash dump under -smp 4). One core's crash
 * report is what matters; a second one competing for the same UART just
 * needs to not corrupt the first. */
static spinlock_t panic_lock = SPINLOCK_INIT;


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
    spin_lock(&panic_lock);   // never released -- see panic_lock's own comment

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

    serial_write_string("CS: ");
    serial_write_hex(regs->cs);
    serial_write_string("\n");

    serial_write_string("RSP: ");
    serial_write_hex(regs->rsp);
    serial_write_string("\n");

    serial_write_string("SS: ");
    serial_write_hex(regs->ss);
    serial_write_string("\n");

    serial_write_string("RBP: ");
    serial_write_hex(regs->rbp);
    serial_write_string("\n");

    // Page Fault (vector 14): CR2 holds the faulting address, and the
    // error code's low bits explain the cause.
    if (regs->vector == 14) {
        uint64_t cr2;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));

        serial_write_string("CR2 (fault addr): ");
        serial_write_hex(cr2);
        serial_write_string("\n");

        // Decode the error code bits (Intel SDM Vol 3A, 4.7 "Page-Fault
        // Exceptions"). Each bit explains one aspect of the fault.
        uint64_t e = regs->error_code;
        serial_write_string("Cause: ");
        serial_write_string((e & 0x1) ? "protection-violation" : "not-present");
        serial_write_string((e & 0x2) ? ", write" : ", read");
        serial_write_string((e & 0x4) ? ", user-mode" : ", kernel-mode");
        if (e & 0x8)  serial_write_string(", reserved-bit-set");
        if (e & 0x10) serial_write_string(", instruction-fetch");
        serial_write_string("\n");
    }

    serial_write_string("system halted.\n");

    // In a real kernel, you would likely halt the system or attempt recovery here
    while (1) {
            __asm__ volatile ("cli; hlt");
        }
    
}