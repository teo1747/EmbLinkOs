# include <stdint.h>
#include "cpu/ioapic.h"
#include "drivers/ata.h"
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
#include "include/io.h"
#include "drivers/pci.h"
#include "drivers/ata.h"
#include "drivers/bootanim.h"
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
    pci_init();
    ioapic_init();
    ata_init();

    kheap_init();
    fb_init();
    console_init();


    // Route keyboard: IRQ 1 = GSI 1 (no override), to vector 33 (keyboard), CPU 0
    ioapic_route(1, 33, 0, false);
    keyboard_init();   // keyboard still on PIC IRQ 1

    __asm__ volatile ("sti");keyboard_init();   // keyboard still on PIC IRQ 1
    boot_animation();           // play the splash

    // Test DMA read
    kprintf("Testing DMA read...\n");
    static uint8_t dma_buf[512] __attribute__((aligned(4)));
    if (ata_read_dma(0, 0, 1, dma_buf) == 0) {
        kprintf("DMA read sector 0: sig %x %x (expect 55 aa)\n",
                (unsigned int)dma_buf[510], (unsigned int)dma_buf[511]);
        kprintf("First 4 bytes: %x %x %x %x\n",
                (unsigned int)dma_buf[0], (unsigned int)dma_buf[1],
                (unsigned int)dma_buf[2], (unsigned int)dma_buf[3]);
    } else {
        kprintf("DMA read FAILED\n");
    }

    // Read boot sector from drive 0 (boot disk)
    uint8_t sector[512];
    if (ata_read_sectors(0, 0, 1, sector) == 0) {
        kprintf("Drive 0 sector 0: sig %x %x\n",
                (unsigned int)sector[510], (unsigned int)sector[511]);
    }

    // Test DMA write + read on data disk (drive 1)
    if (ata_drive_count() > 1) {
        static uint8_t dma_wbuf[512] __attribute__((aligned(4)));
        static uint8_t dma_rbuf[512] __attribute__((aligned(4)));
        for (int i = 0; i < 512; i++) dma_wbuf[i] = (uint8_t)(0xA0 + (i & 0x1F));

        kprintf("Testing DMA write+read...\n");
        int w = ata_write_dma(1, 200, 1, dma_wbuf);
        int r = ata_read_dma(1, 200, 1, dma_rbuf);

        if (w == 0 && r == 0) {
            bool match = true;
            for (int i = 0; i < 512; i++) {
                if (dma_wbuf[i] != dma_rbuf[i]) { match = false; break; }
            }
            kprintf("DMA write+read test: %s\n", match ? "PASS" : "FAIL");
        } else {
            kprintf("DMA write/read failed (w=%d r=%d)\n", w, r);
        }
    }

    // Test write+read on the data disk (drive 1) if present
    if (ata_drive_count() > 1) {
        uint8_t wbuf[512], rbuf[512];
        for (int i = 0; i < 512; i++) wbuf[i] = (uint8_t)(i & 0xFF);

        if (ata_write_sectors(1, 100, 1, wbuf) == 0 &&
            ata_read_sectors(1, 100, 1, rbuf) == 0) {
            // Verify
            bool match = true;
            for (int i = 0; i < 512; i++) {
                if (wbuf[i] != rbuf[i]) { match = false; break; }
            }
            kprintf("Data disk write/read test: %s\n", match ? "PASS" : "FAIL");
            
        }
    }
    
    
    fb_clear(0, 0, 64);   // dark blue screen — if you see this, framebuffer works
    kprintf("Helios — framebuffer alive\n");
    //timer_init();
   

     // DON'T call timer_init() — replaced by LAPIC timer
    lapic_timer_init(48);

    

    // fully mask the PIC 8259A interrupt controllers
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);

    



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