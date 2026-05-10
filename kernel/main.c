# include <stdint.h>
#include "drivers/serial.h"
#include "../kernel/cpu/idt.h"

// VGA text mode buffer
#define VGA_ADDR ((volatile uint16_t*) 0xB8000)
#define VGA_COLS 80
#define VGA_ROWS 25

static int col = 0;
static int row = 0;


static void vga_putchar(char c, uint8_t color) {
    if (c == '\n') {
        col = 0;
        row++;
        return;
    }
    VGA_ADDR[row * VGA_COLS + col] = 
                             (uint16_t)c | (uint16_t)(color << 8);  // Write character and color to VGA buffer
    col++;
    if (col >= VGA_COLS) {
        col = 0;
        row++;
    }

}


static void vga_print(const char* str, uint8_t color) {
    while (*str) {
        vga_putchar(*str++, color);
    }
}


static void vga_clear(void) {
    for (int i = 0; i < VGA_COLS * VGA_ROWS; i++) {
        VGA_ADDR[i] = (uint16_t)' ' | (uint16_t)(0x0F << 8);  // Clear screen with spaces
    }
    col = 0;
    row = 0;
}


void kernel_main(void) {

    serial_init();  // Initialize serial communication
   
    vga_clear();  // Clear screen with white on black
    vga_print("MyOS kernel\n", 0x0F);  // Print message in white on black
    vga_print("64-bit Long Mode\n", 0x0F); 
    vga_print("Kernel loaded successfully!\n", 0x0B);

    serial_write_string("MyOS kernel initialized.\n");  // Print message via serial port
    serial_write_string("Serial output working!\n");

    idt_init();     // Initialize the Interrupt Descriptor Table (IDT)
    serial_write_string("IDT loaded\n");

    // Trigger a test exception (divide by zero) to verify IDT and ISR handling
    serial_write_string("Triggering divide by zero exception...\n");
    int a = 10;
    int b = 0;
    int c = a / b; // This will cause a divide by zero exception
    (void)c; // Suppress unused variable warning

    serial_write_string("this should never print\n");

    for(;;) {
        // Infinite loop to keep the kernel running
    }
}


