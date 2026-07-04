#include <stdint.h>
#include "include/types.h"
#include "include/kprintf.h"
#include "include/io.h"
#include "include/errno.h"
#include "include/kstring.h"

#include "drivers/serial.h"
#include "drivers/framebuffer.h"
#include "drivers/gpu.h"
#include "drivers/console.h"
#include "drivers/keyboard.h"
#include "drivers/timer.h"
#include "drivers/hpet.h"
#include "drivers/pci.h"
#include "drivers/usb.h"
#include "drivers/ata.h"
#include "drivers/ahci.h"
#include "drivers/bootanim.h"

#include "cpu/gdt.h"
#include "cpu/idt.h"
#include "cpu/syscall.h"
#include "cpu/pic.h"
#include "cpu/irq.h"
#include "cpu/lapic.h"
#include "cpu/ioapic.h"

#include "mm/pmm.h"
#include "mm/vmm.h"
#include "mm/kheap.h"

#include "acpi/acpi.h"
#include "block/block.h"
#include "block/partition.h"
#include "fs/fat32.h"
#include "fs/embkfs/embkfs.h"
#include "fs/vfs.h"
#include "fs/fd.h"
#include "selftests.h"

#include "process/process.h"


extern uint64_t lapic_timer_get_ticks(void);

static void kernel_handle_line_command(const char *cmd)
{
    if (selftests_handle_command(cmd))
        return;

    if (cmd[0])
        kprintf("\n[cmd] unknown command: %s\n", cmd);
}

void kernel_main(void) {
    // --- Core init ---
    serial_init();
    gdt_init();
    idt_init();
    syscall_init();   // install int 0x80 (DPL3) + #DF on IST1; needs idt_init first
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

    // --- HPET: must come after acpi_init + vmm (for MMIO map) ---
    hpet_init();

    // --- Devices ---
    
    pci_init();
    usb_init();
    ata_init();    // registers ATA drives as block devices internally
    ahci_init();   // runs IDENTIFY per port, stores sector counts

    // Register AHCI drives as block devices (after ahci_init filled sector counts)
    ahci_register_block_devices();



    // --- Display + input ---
    gpu_init();   // pick VirtIO-GPU / Bochs DISPI before the fb comes up
    fb_init();
    console_init();
    keyboard_init();
    ioapic_route(1, 33, 0, false);   // keyboard GSI 1 -> vector 33 -> CPU 0

    // --- Timer (LAPIC) + retire PIC ---
    lapic_timer_init(48);
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);

    // --- TSC calibration (uses HPET if available, else PIT fallback) ---
    tsc_calibrate();

    __asm__ volatile ("sti");

    // --- Boot splash ---
    boot_animation();
    console_clear();

// ============================================================
    //  Block device enumeration
    // ============================================================
    // Probe each whole disk for an MBR and expose its partitions (sda1, sda2,
    // ...) as block devices. Must run after `sti` above: the ATA read path is
    // IRQ-driven and would hang waiting on an interrupt that can't fire yet.
    // Done before enumeration/mount so partitions appear in the listing and the
    // mount probe below sees them alongside whole disks.
    embk_partition_scan_all();

    kprintf("\n=== Block devices ===\n");
    for (uint32_t i = 0; i < embk_block_count(); i++) {
        struct embk_block_device *dev = embk_block_get(i);
        kprintf("  %s: %u blocks (%u KB)\n",
                dev->name,
                (unsigned int)dev->block_count,
                (unsigned int)((dev->block_count * dev->block_size) / 1024));
    }

    // ============================================================
    //  Mount FAT32 (probe every disk, mount the first valid one)
    // ============================================================
    embkfs_init();
    static struct fat32_volume vol;
    bool found = false;
    for (uint32_t i = 0; i < embk_block_count(); i++) {
        struct embk_block_device *d = embk_block_get(i);
        if (fat32_mount(d, &vol) == EMBK_OK) {
            found = true;
            break;
        }
    }
    selftests_init(&vol, found);
    if (!found) {
        kprintf("No FAT32 volume found on any disk\n");
    }

    vfs_init();
    bool vfs_ready = false;
    struct embkfs_volume *embk_live = embkfs_live_volume();

    if (embk_live) {
        int rc = embkfs_vfs_register("/", embk_live);
        if (rc != EMBK_OK) {
            kprintf("VFS: EMBKFS register failed: %s\n", embk_strerror(rc));
        } else {
            vfs_ready = true;
        }
    }

    if (found) {
        const char *fat_mp = vfs_ready ? "/fat32" : "/";
        int rc = fat32_vfs_register(fat_mp, &vol);
        if (rc != EMBK_OK) {
            kprintf("VFS: FAT32 register at %s failed: %s\n", fat_mp, embk_strerror(rc));
        } else {
            vfs_ready = true;
        }
    }

    if (!vfs_ready)
        kprintf("VFS: no filesystem mounted\n");

    selftests_set_vfs_ready(vfs_ready);
    if (vfs_ready)
        vfs_fd_init();

    vfs_ls("/");



    // Main loop: keyboard echo + tick heartbeat
    uint64_t last = 0;
    char cmd_buf[128];
    uint32_t cmd_len = 0;
    for (;;) {
        uint64_t now = lapic_timer_get_ticks();
        if (now >= last + 500) { last = now; }
        // Legacy USB HCs (UHCI/OHCI/EHCI) are polled: drain any completed
        // interrupt-IN transfers and re-arm them. xHCI input is IRQ-driven.
        usb_poll();
        // USB HID input now arrives via the xHCI interrupt (see xhci_enable_irq),
        // which wakes us from the hlt below and injects keys into the keyboard buffer.
        if (keyboard_has_char()) {
            char c = keyboard_getchar();
            kprintf("%c", c);

            if (c == '\r' || c == '\n') {
                cmd_buf[cmd_len] = '\0';
                kernel_handle_line_command(cmd_buf);
                cmd_len = 0;
            } else if ((c == '\b' || c == 127) && cmd_len > 0) {
                cmd_len--;
            } else if (c >= 32 && c <= 126) {
                if (cmd_len + 1 < sizeof cmd_buf)
                    cmd_buf[cmd_len++] = c;
            }
        }
            /* ... vmm, acpi, drivers, fs mount, selftests ... */
        process_init();
        int pid = process_create("/init.elf");
        if (pid < 0) {
            serial_write_string("failed to create init process\n");
        } else {
            process_start_first();   /* one-way; never returns */
        }
        /* nothing meaningful runs here */
        for (;;) __asm__ volatile("hlt");
        /* nothing here runs */
        __asm__ volatile ("hlt");   // wake on any IRQ (timer, PS/2, or xHCI)
    }
}