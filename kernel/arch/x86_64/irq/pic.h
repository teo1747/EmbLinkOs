#ifndef _PIC_H_
#define _PIC_H_

#include <stdint.h>
#include "include/types.h"   /* bool */


// PIC ports
#define PIC1_COMMAND    0x20
#define PIC1_DATA       0x21
#define PIC2_COMMAND    0xA0
#define PIC2_DATA       0xA1

//EOI command byte
#define PIC_EOI         0x20

// Remapping PIC interrupts to avoid conflicts with CPU exceptions
#define PIC1_VECTOR_OFFSET     0x20  // IRQs 0-7 Master
#define PIC2_VECTOR_OFFSET     0x28  // IRQs 8-15 slave

// Remap both PICS to vector offsets defined above, make all IRQS INITIALLY
void pic_init(void);

// Send End_OF_Interrupt for given IRQ number (0_15)
void pic_send_eoi(uint8_t irq);

// Mask (disable) a specific IRQ line (0-15)
void pic_mask_irq(uint8_t irq);

// Unmask (enable) a specific IRQ line (0-15)
void pic_unmask_irq(uint8_t irq);

/* Read a PIC's Interrupt Mask Register (IMR) -- the currently masked lines.
 * master=true reads PIC1 (IRQ0-7), false reads PIC2 (IRQ8-15). Used by
 * irq_register/unregister to SAVE and RESTORE mask state rather than blindly
 * unmasking/masking. */
uint8_t pic_get_mask(bool master);

/* Is a delivered IRQ7/IRQ15 a SPURIOUS 8259 interrupt? The classic quirk: a
 * line that de-asserts between the PIC latching it and the CPU's interrupt-ack
 * makes the chip report its lowest-priority line (7 on the master, 15 on the
 * slave) with the In-Service Register bit NOT set -- there is no real IRQ to
 * service. Reads the ISR via OCW3 and returns true iff the ISR bit is clear.
 * Only meaningful for irq==7 / irq==15; returns false otherwise. */
bool pic_irq_is_spurious(uint8_t irq);

#endif /* _PIC_H_ */