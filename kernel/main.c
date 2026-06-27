#include <stdint.h>
#include "include/types.h"
#include "include/kprintf.h"
#include "include/io.h"
#include "include/errno.h"
#include "include/kstring.h"

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
#include "fs/fat32.h"
#include "fs/embkfs/embkfs.h"


extern uint64_t lapic_timer_get_ticks(void);

static void fat32_read_path(struct fat32_volume *vol, const char *path, uint8_t *buffer, uint32_t buffer_size) {
    int n = fat32_read(vol, path, buffer, buffer_size - 1);
    if (n >= 0) {
        buffer[n] = '\0';
        kprintf("READ %s: %d bytes\n%s\n", path, n, buffer);
    } else {
        kprintf("READ %s failed: %s\n", path, embk_strerror(n));
    }
}

static void fat32_write_path(struct fat32_volume *vol, const char *path,
                             const char *content) {
    uint32_t len = 0;
    while (content[len]) {
        len++;
    }
    int rc = fat32_write(vol, path, (const uint8_t *)content, len);
    if (rc >= 0) {
        kprintf("WROTE %s: %d bytes\n", path, rc);
    } else {
        kprintf("WRITE %s failed: %s\n", path, embk_strerror(rc));
    }
}

static void fat32_test_all(struct fat32_volume *vol) {
    static uint8_t buffer[512];

    kprintf("\n=== FAT32 TEST SUITE ===\n");
    fat32_list_root(vol);

    fat32_read_path(vol, "HELLO.TXT", buffer, sizeof(buffer));
    fat32_read_path(vol, "/SUBDIR/INSIDE.TXT", buffer, sizeof(buffer));

    fat32_write_path(vol, "/NEWFILE.TXT", "This is a new file written by test.");
    fat32_read_path(vol, "/NEWFILE.TXT", buffer, sizeof(buffer));

    int rc = fat32_mkdir(vol, "/TESTDIR");
    if (rc == EMBK_OK) {
        kprintf("MKDIR /TESTDIR succeeded\n");
    } else if (rc == -EMBK_EEXIST) {
        kprintf("MKDIR /TESTDIR skipped: already exists\n");
    } else {
        kprintf("MKDIR /TESTDIR failed: %s\n", embk_strerror(rc));
    }

    fat32_write_path(vol, "/TESTDIR/INNER.TXT", "Inside test dir file.");
    fat32_read_path(vol, "/TESTDIR/INNER.TXT", buffer, sizeof(buffer));

    fat32_write_path(vol, "/LONG NAME EXAMPLE.TXT", "Long filename test content.");
    fat32_read_path(vol, "/LONG NAME EXAMPLE.TXT", buffer, sizeof(buffer));

    kprintf("=== FAT32 TEST SUITE COMPLETE ===\n");
}

static void kernel_handle_line_command(const char *cmd)
{
    if (strcmp(cmd, "embkfs-test") == 0) {
        int rc_path = embkfs_run_path_selftests();
        int rc_alloc = embkfs_run_allocator_selftests();
        int rc_tree = embkfs_run_tree_selftests();
        int rc_obj = embkfs_run_object_selftests();
        int rc_ns = embkfs_run_namespace_selftests();
        if (rc_path == EMBK_OK && rc_alloc == EMBK_OK && rc_tree == EMBK_OK && rc_obj == EMBK_OK && rc_ns == EMBK_OK) {
            kprintf("\n[cmd] embkfs-test: OK\n");
        } else {
            if (rc_path != EMBK_OK)
                kprintf("\n[cmd] embkfs-test path failed: %s\n", embk_strerror(rc_path));
            if (rc_alloc != EMBK_OK)
                kprintf("\n[cmd] embkfs-test allocator failed: %s\n", embk_strerror(rc_alloc));
            if (rc_tree != EMBK_OK)
                kprintf("\n[cmd] embkfs-test tree failed: %s\n", embk_strerror(rc_tree));
            if (rc_obj != EMBK_OK)
                kprintf("\n[cmd] embkfs-test object failed: %s\n", embk_strerror(rc_obj));
            if (rc_ns != EMBK_OK)
                kprintf("\n[cmd] embkfs-test namespace failed: %s\n", embk_strerror(rc_ns));
        }
        return;
    }

    if (cmd[0])
        kprintf("\n[cmd] unknown command: %s\n", cmd);
}

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
    //  Block device enumeration
    // ============================================================
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
    if (!found) {
        kprintf("No FAT32 volume found on any disk\n");
    } else {
        fat32_test_all(&vol);
    }

    // Main loop: keyboard echo + tick heartbeat
    uint64_t last = 0;
    char cmd_buf[128];
    uint32_t cmd_len = 0;
    for (;;) {
        uint64_t now = lapic_timer_get_ticks();
        if (now >= last + 500) { last = now; }
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
        __asm__ volatile ("hlt");
    }
}