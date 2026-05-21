#include "pit.h"
#include "../include/io.h"

#define PIT_FREQUENCY 1193182  // PIT input clock frequency in Hz


// PIT channel 2 is gated by bit 0 of port 0x61. We will use channel 2 in mode 0 (one-shot) to implement a microsecond delay function.
// can be read at bit 5 of port 0x61 (0 = ready, 1 = counting)

void pit_delay_ms(uint32_t ms){
    uint32_t count = (PIT_FREQUENCY / 1000) * ms; // Calculate the count value for the desired delay

    if (count > 0xFFFF) {
        count = 0xFFFF; // Max count for 16-bit counter
    }

     // Enable channel 2 gate by setting bit 0 of port 0x61
    uint8_t port61 = inb(0x61);
    port61 |= (port61 & ~0x02) | 0x01; // Set bit 0 to enable gate
    outb(0x61, port61);
    // Send command byte: channel 2, access mode lobyte/hibyte, mode 0 (one-shot), binary counting
    outb(0x43, 0xB4);

    // Send count value (low byte first)
    outb(0x43,  0xB0); // Channel 2, access mode lobyte/hibyte, mode 0 (one-shot), binary counting
    outb(0x42, count & 0xFF); // low byte
    outb(0x42, (count >> 8) & 0xFF); // high byte


    // Restart the counter by toggling bit 0 of port 0x61
    port61 = inb(0x61) & ~0x01; // Clear bit 0 to disable gate
    outb(0x61, port61);
    port61 |= 0x01; // Set bit 0 to enable gate and start counting
    outb(0x61, port61);

    // Wait for the PIT to finish counting by polling bit 5 of port 0x61
    while (!(inb(0x61) & 0x20)) {
        // Still counting
    }
}