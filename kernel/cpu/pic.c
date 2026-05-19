#include "pic.h"
#include "../include/io.h"
#include "../drivers/serial.h"

// ICW1 bits
#define ICW1_ICW4       0x01    // ICW4 (not needed if single PIC mode)
#define ICW1_INIT       0x10    // Initialization - required!

// ICW4 bits
#define ICW4_8086       0x01    // 8086 mode not (8080)


// A small between PIC writes. older hardware needs the chip
// to settle between port writes. Writing to an anuxiliary port is a common way to add a small delay.

static inline void io_wait(void) {
    outb(0x80, 0); // write to an unused port to add a small delay
}


void pic_init(void) {
    serial_write_string("\n=== PIC init ===\n");

    
    // Start initialization in cascade mode
    outb(PIC1_COMMAND, ICW1_INIT | ICW1_ICW4);  // Master PIC
    io_wait();
    outb(PIC2_COMMAND, ICW1_INIT | ICW1_ICW4);  // Slave PIC
    io_wait();

    // Set vector offset for master and slave PICs
    outb(PIC1_DATA, PIC1_VECTOR_OFFSET);  // Master PIC vector offset IRQ 0 - 7 -> 0x20 - 0x27
    io_wait();
    outb(PIC2_DATA, PIC2_VECTOR_OFFSET);  // Slave PIC vector offset IRQ 8 - 15 -> 0x28 - 0x2F
    io_wait();

    // Tell Master PIC that there is a slave PIC at IRQ2 (0000 0100)
    outb(PIC1_DATA, 0x04);
    io_wait();

    // Tell Slave PIC its cascade identity (0000 0010)
    outb(PIC2_DATA, 0x02);
    io_wait();

    // Set both PICs to operate in 8086 mode
    outb(PIC1_DATA, ICW4_8086);
    io_wait();
    outb(PIC2_DATA, ICW4_8086);
    io_wait();

    // Mask all IRQs initially (disable all interrupts) driver will unmask specific IRQs as needed
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);

    serial_write_string("PIC remapped: master 0x20 - 0x27 and slave 0x28 - 0x2F \n");
    serial_write_string("ALL IRQs masked\n");
}

void pic_send_eoi(uint8_t irq) {
    if (irq >= 8) {
        outb(PIC2_COMMAND, PIC_EOI); // Send EOI to slave PIC for IRQs 8-15
    }
    outb(PIC1_COMMAND, PIC_EOI); // Always send EOI to master PIC
}


void pic_mask_irq(uint8_t irq) {
    uint16_t port;
    uint8_t value;

    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq -= 8; // Adjust for slave PIC
    }

    value = inb(port) | (1 << irq); // Set the bit to mask
    outb(port, value);
}

void pic_unmask_irq(uint8_t irq) {
    uint16_t port;
    uint8_t value;

    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq -= 8; // Adjust for slave PIC
    }

    value = inb(port) & ~(1 << irq); // Clear the bit to unmask
    outb(port, value);
}

