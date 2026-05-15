# include <stdint.h>
#include "drivers/serial.h"
#include "../kernel/cpu/idt.h"
#include "mm/pmm.h"
#include "mm/vmm.h"

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
    vga_print("Helios kernel\n", 0x0F);  // Print message in white on black
    

    idt_init();     // Initialize the Interrupt Descriptor Table (IDT)
    serial_write_string("IDT loaded\n");

    pmm_init();
    vmm_init();
   // Test: allocate a new page and map it to a fresh virtual address
    serial_write_string("\n=== VMM Test ===\n");
    
    uint64_t phys = pmm_alloc_page();
    uint64_t virt = 0xFFFFFFFF90000000;  // somewhere new in higher half
    
    serial_write_string("Allocated phys: ");
    serial_write_hex(phys);
    serial_write_string("\nMapping to virt: ");
    serial_write_hex(virt);
    serial_write_string("\n");
    
    vmm_map(virt, phys, VMM_WRITABLE);
    
    // Write to it
    uint64_t *ptr = (uint64_t *)virt;
    *ptr = 0xCAFEBABEDEADBEEF;
    
    serial_write_string("Wrote magic value\n");
    serial_write_string("Read back: ");
    serial_write_hex(*ptr);
    serial_write_string("\n");
    
    // Verify translation
    uint64_t actual_phys = vmm_get_phys(virt);
    serial_write_string("vmm_get_phys returned: ");
    serial_write_hex(actual_phys);
    serial_write_string("\n");

    for(;;);
}


