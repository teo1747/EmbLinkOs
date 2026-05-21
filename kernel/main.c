# include <stdint.h>
#include "include/types.h"
#include "drivers/serial.h"
#include "../kernel/cpu/idt.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "include/kprintf.h"
#include "drivers/framebuffer.h"
#include "drivers/console.h"
#include "cpu/pic.h"
#include "cpu/irq.h"
#include "drivers/timer.h"
#include "cpu/gdt.h"
#include "drivers/keyboard.h"
#include "mm/kheap.h"
#include "acpi/acpi.h"
#include "cpu/lapic.h"
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
    gdt_init();
    idt_init();
    serial_write_string("IDT loaded\n");

    pic_init();
    irq_install();
    pmm_init();
    vmm_init();
    acpi_init();
    lapic_init();
    kheap_init();
    fb_init();
    console_init();
    //timer_init();
   

     // DON'T call timer_init() — replaced by LAPIC timer
    lapic_timer_init(48);

    keyboard_init();   // keyboard still on PIC IRQ 1

    __asm__ volatile ("sti");

    // Verify LAPIC timer is ticking
    kprintf("Waiting for LAPIC timer ticks...\n");
    extern uint64_t lapic_timer_get_ticks(void);
    uint64_t last = 0;
    for (;;) {
        uint64_t now = lapic_timer_get_ticks();
        if (now >= last + 100) {   // every ~1 sec at 100 Hz
            kprintf("LAPIC tick %u\n", (unsigned int)now);
            last = now;
        }
        // also echo keyboard
        if (keyboard_has_char()) {
            kprintf("%c", keyboard_getchar());
        }
        __asm__ volatile ("hlt");
    }
}