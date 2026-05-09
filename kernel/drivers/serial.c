#include "serial.h"
#include "../include/io.h"


#define SERIAL_PORT 0x3F8 // COM1 port

void serial_init(void) {
    outb(SERIAL_PORT + 1, 0x00); // Disable all interrupts
    outb(SERIAL_PORT + 3, 0x80); // Enable DLAB (set baud rate divisor)
    outb(SERIAL_PORT + 0, 0x01); // Set divisor to 1 (lo byte) 115200 baud
    outb(SERIAL_PORT + 1, 0x00); //                  (hi byte)
    outb(SERIAL_PORT + 3, 0x03); // 8 bits, no parity, one stop bit
    outb(SERIAL_PORT + 2, 0xC7); // Enable FIFO, clear them, with 14-byte threshold
    outb(SERIAL_PORT + 4, 0x0B); // IRQs enabled, RTS/DSR set
}



//helper function

static int serial_is_ready() {
    return inb(SERIAL_PORT + 5) & 0x20;
}


void serial_write_char(char c) {
    // Wait for the transmit buffer to be empty
    while (!serial_is_ready());
    outb(SERIAL_PORT, c);
}


void serial_write_string(const char *str) {
    while (*str) {
        serial_write_char(*str++);
    }
}





