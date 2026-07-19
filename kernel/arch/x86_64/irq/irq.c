#include "arch/x86_64/irq/irq.h"
#include "arch/x86_64/irq/lapic.h"
#include "arch/x86_64/irq/pic.h"
#include "arch/x86_64/irq/idt.h"
#include "drivers/char/serial.h"

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

/* Prior PIC mask state, captured the first time irq_register() touches a line
 * so irq_unregister() can RESTORE it rather than unconditionally masking.
 * Without this, register-then-unregister is not transparent: it would leave a
 * line masked even if it had been unmasked before, silently disabling whatever
 * else relied on it. `saved` gates validity (both zero-init => "nothing saved
 * yet"); `was_masked` is the captured bit. */
static bool irq_mask_saved[16];
static bool irq_was_masked[16];


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

    /* Save the line's prior mask bit ONCE, before we unmask, so unregister can
     * put it back exactly. `irq < 8` selects master vs slave; the bit within
     * that register is `irq & 7`. */
    if (!irq_mask_saved[irq]) {
        uint8_t imr = pic_get_mask(irq < 8);
        irq_was_masked[irq] = (imr & (1 << (irq & 7))) != 0;
        irq_mask_saved[irq] = true;
    }

    pic_unmask_irq(irq); // Unmask the IRQ at the PIC
}


void irq_unregister(uint8_t irq) {
    if (irq >= 16) return; // Invalid IRQ number
    irq_handlers[irq] = 0;

    /* Restore the mask state we captured at register time instead of blindly
     * masking. If there is nothing saved (unregister with no matching register),
     * fall back to masking -- the safe default for a line with no handler. */
    if (irq_mask_saved[irq]) {
        if (irq_was_masked[irq]) pic_mask_irq(irq);
        else                     pic_unmask_irq(irq);
        irq_mask_saved[irq] = false;
    } else {
        pic_mask_irq(irq);
    }
}


// called from assembly stub with IRQ number in rdi
void irq_handler(struct registers *regs) {
    uint8_t irq = (uint8_t)(regs->vector - 32); // IRQ number is vector - 32

    /* Spurious 8259 IRQ7/15 guard. Gated on there being NO registered handler:
     * a registered handler means a REAL device owns this vector via the IO-APIC
     * (ATA secondary is IRQ15 -> vector 47), and its interrupts never touch the
     * PIC, so the PIC ISR bit would read clear for them -- consulting it there
     * would wrongly drop a genuine ATA IRQ. Only an UNWIRED 7/15 can be a PIC
     * phantom worth swallowing. Dormant today (the PIC is masked, so it delivers
     * nothing), but correct the moment a PIC line is ever used again: a master
     * spurious (7) gets no EOI at all; a slave spurious (15) still needs the
     * master's cascade EOI even though the slave must not be acked. Neither
     * takes the LAPIC EOI below -- nothing was delivered through the LAPIC. */
    if ((irq == 7 || irq == 15) && !irq_handlers[irq] && pic_irq_is_spurious(irq)) {
        if (irq == 15) pic_send_eoi(8);   // cascade EOI to the master only
        return;
    }

    if (irq_handlers[irq]) {
        irq_handlers[irq](); // Call the registered handler
    }else {
        serial_write_string("[Unhandled IRQ: ");
        serial_write_hex(irq);
        serial_write_string("]\n");
    }

    lapic_send_eoi(); // Send End of Interrupt to IO-APIC
}


