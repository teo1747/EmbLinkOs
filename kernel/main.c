# include <stdint.h>
#include "drivers/serial.h"
#include "../kernel/cpu/idt.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "include/kprintf.h"
#include "drivers/framebuffer.h"
#include "drivers/console.h"

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

   serial_init();

    idt_init();
    serial_write_string("IDT loaded\n");

    pmm_init();
    vmm_init();
    fb_init();
    console_init();

    // Now kprintf goes to both serial AND framebuffer
    kprintf("=== Helios Kernel ===\n");
    kprintf("Phase 6.3: Console abstraction\n");
    kprintf("\n");

    kprintf("Signed:    %d\n", -42);
    kprintf("Unsigned:  %u\n", 100);
    kprintf("Hex:       %x\n", 0xCAFE);
    kprintf("HEX:       %X\n", 0xDEADBEEF);
    kprintf("Pointer:   %p\n", &kernel_main);
    kprintf("String:    %s\n", "hello world");
    kprintf("Char:      %c\n", 'A');
    kprintf("Long hex:  %lx\n", (uint64_t)0xCAFEBABEDEADBEEF);
    kprintf("Padded:    %08x\n", 0xABCD);
    kprintf("Percent:   %%\n");
    kprintf("\nVMM PML4 at phys: %p\n", (void *)vmm_get_kernel_pml4());

    for (;;);
}


