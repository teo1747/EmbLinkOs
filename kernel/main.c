#include <stdint.h>
#include "include/types.h"
#include "include/kprintf.h"
#include "include/io.h"
#include "include/errno.h"

#include "drivers/serial.h"
#include "drivers/framebuffer.h"
#include "drivers/console.h"
#include "drivers/keyboard.h"
#include "drivers/timer.h"
#include "drivers/pci.h"
#include "drivers/ata.h"
#include "drivers/ahci.h"
#include "drivers/bootanim.h"

#include "cpu/gdt.h"
#include "cpu/idt.h"
#include "cpu/pic.h"
#include "cpu/irq.h"
#include "cpu/lapic.h"
#include "cpu/ioapic.h"

#include "mm/pmm.h"
#include "mm/vmm.h"
#include "mm/kheap.h"

#include "acpi/acpi.h"
#include "block/block.h"


extern uint64_t lapic_timer_get_ticks(void);


void kernel_main(void) {
    // --- Core init ---
    serial_init();
    gdt_init();
    idt_init();
    pic_init();
    irq_install();

    // --- Memory ---
    pmm_init();
    vmm_init();
    kheap_init();

    // --- Interrupt controllers (ACPI -> LAPIC -> IO-APIC) ---
    acpi_init();
    lapic_init();
    ioapic_init();

    // --- Devices ---
    pci_init();
    ata_init();    // registers ATA drives as block devices internally
    ahci_init();   // runs IDENTIFY per port, stores sector counts

    // Register AHCI drives as block devices (after ahci_init filled sector counts)
    ahci_register_block_devices();

    // --- Display + input ---
    fb_init();
    console_init();
    keyboard_init();
    ioapic_route(1, 33, 0, false);   // keyboard GSI 1 -> vector 33 -> CPU 0

    // --- Timer (LAPIC) + retire PIC ---
    lapic_timer_init(48);
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);

    __asm__ volatile ("sti");

    // --- Boot splash ---
    boot_animation();

    // ============================================================
    //  BLOCK LAYER TEST
    // ============================================================
    kprintf("\n=== Block devices ===\n");
    for (uint32_t i = 0; i < embk_block_count(); i++) {
        struct embk_block_device *dev = embk_block_get(i);
        kprintf("  %s: %u blocks (%u KB)\n",
                dev->name,
                (unsigned int)dev->block_count,
                (unsigned int)((dev->block_count * dev->block_size) / 1024));
    }

    // Read sector 0 of sda through the GENERIC interface (don't care which driver)
    struct embk_block_device *sda = embk_block_get_by_name("sda");
    if (sda) {
        static uint8_t buf[512] __attribute__((aligned(4)));
        int ret = embk_block_read(sda, 0, 1, buf);
        if (ret == EMBK_OK) {
            kprintf("sda sector 0 via block layer: sig %x %x (expect 55 aa)\n",
                    (unsigned int)buf[510], (unsigned int)buf[511]);
        } else {
            kprintf("sda read failed: %s\n", embk_strerror(ret));
        }
    }

    // Write+read round-trip on the AHCI disk THROUGH THE BLOCK LAYER.
    // sdc = third device = AHCI disk (sda/sdb are the two IDE drives).
    struct embk_block_device *sdc = embk_block_get_by_name("sdc");
    if (sdc) {
        static uint8_t wbuf[512] __attribute__((aligned(4)));
        static uint8_t rbuf[512] __attribute__((aligned(4)));
        const char *msg = "WRITTEN VIA EMBK BLOCK LAYER";
        for (int i = 0; i < 512; i++) wbuf[i] = 0;
        for (int i = 0; msg[i]; i++) wbuf[i] = (uint8_t)msg[i];

        int wr = embk_block_write(sdc, 50, 1, wbuf);
        int rd = embk_block_read(sdc, 50, 1, rbuf);
        if (wr == EMBK_OK && rd == EMBK_OK) {
            bool match = true;
            for (int i = 0; i < 512; i++) {
                if (wbuf[i] != rbuf[i]) { match = false; break; }
            }
            kprintf("Block-layer write+read on %s: %s\n",
                    sdc->name, match ? "PASS" : "FAIL");
        } else {
            kprintf("Block-layer w/r failed: write=%s read=%s\n",
                    embk_strerror(wr), embk_strerror(rd));
        }
    }
    // ============================================================

    kprintf("\nEmBlink OS ready.\n");

    // Main loop: keyboard echo + LAPIC tick heartbeat
    uint64_t last = 0;
    for (;;) {
        uint64_t now = lapic_timer_get_ticks();
        if (now >= last + 500) {   // heartbeat every ~5s (quieter than before)
            last = now;
        }
        if (keyboard_has_char()) {
            kprintf("%c", keyboard_getchar());
        }
        __asm__ volatile ("hlt");
    }
}