#ifndef _PIC_H_
#define _PIC_H_

#include <stdint.h>


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

#endif /* _PIC_H_ */