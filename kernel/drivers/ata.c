#include "ata.h"
#include "../include/io.h"
#include "../include/kprintf.h"
#include "../drivers/serial.h"
#include "../cpu/irq.h"
#include "../cpu/ioapic.h"

#include <stdint.h>

#define ATA_PRIMARY_IRQ  14
#define ATA_PRIMARY_VECTOR 46   // IDT vector for IRQ14 (32 + 14)

static volatile bool ata_irq_fired = false;

static struct ata_drive drives[ATA_MAX_DRIVES];
static uint32_t drive_count = 0;
volatile uint64_t ata_irq_count = 0;


static void ata_irq_handler(void) {
    // Reading the status register acknowledge the interrupt at the drive.
    // Without this, the controller never sends another interrupt.
    inb(ATA_PRIMARY_IO + ATA_REG_STATUS);
    ata_irq_count++;
    ata_irq_fired = true;
    // LAPIC EOI (End Of Interrupt) is sent by the common irq_handler() after return.
}

static int ata_wait_irq(void){
    // Sleep until the ATA IRQ sets the flag. the 100 hz timer guarantees
    // We wake periodically to re-check even if we miss the exact moment.
    int timeout = 0;
    while (!ata_irq_fired) {
        __asm__ volatile ("hlt");
        if (++timeout > 1000000) {
        kprintf("ATA IRQ timeout\n");
        return -1; // safety: never hang forever on bad hardware
        }
    }
    ata_irq_fired = false;
    return 0;
}


// 400ns delY: READ ALTERNATE STATUS 4 TIMESS (~100ns each)

static void ata_io_wait(uint16_t ctrl_base) {
    inb(ctrl_base);
    inb(ctrl_base);
    inb(ctrl_base); // 400ns delay
    inb(ctrl_base);
}


// SPIN until BSY clears. Returns 0 on success, -1 on error.
static int ata_wait_not_busy(uint16_t io_base) { 
    // Generous timeout to aviod hanging forever on bad hardware
    for (int i = 0; i < 100000; i++) { 
        if (!(inb(io_base + ATA_REG_STATUS) & ATA_STATUS_BSY)) { 
            return 0; 
        }
        
    }
    return -1;
}


// Spin until DRQ sets (data ready). Returns 0 on success, -1 on error.
static int ata_wait_drq(uint16_t io_base) {
    for (int i = 0; i < 100000; i++) {
        uint8_t status = inb(io_base + ATA_REG_STATUS);
        if (status & ATA_STATUS_ERR) {
            return -1;
        }
        if (!(status & ATA_STATUS_BSY) && (status & ATA_STATUS_DRQ)) {
            return 0;
        }
  
    }
  return -1;
}


// Try to detect one drive. Return true if found, false if not.
static bool ata_detect(uint16_t io_base, uint16_t ctrl_base, bool is_slave, struct ata_drive *out) {
    // Select drive: 0xA0 slave (non-LBA select for IDENTIFY)
    outb(io_base + ATA_REG_DRIVE, is_slave ? 0xB0 : 0xA0);
    ata_io_wait(ctrl_base);

    // Zero out the LBA/count registers, then send Identify command
    outb(io_base + ATA_REG_SECCOUNT, 0);
    outb(io_base + ATA_REG_LBA_LOW, 0); 
    outb(io_base + ATA_REG_LBA_MID, 0); 
    outb(io_base + ATA_REG_LBA_HIGH, 0); 
    outb(io_base + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);

    uint8_t status = inb(io_base + ATA_REG_STATUS);
    if (status == 0) {
        return false;
    }

    if (ata_wait_not_busy(io_base) < 0) {
        return false;
    }

    // ATAPI/SATA signature check
    if (inb(io_base + ATA_REG_LBA_MID) != 0 || inb(io_base + ATA_REG_LBA_HIGH) != 0) {
        return false; // Not a plain ATA drive
    }

    if (ata_wait_drq(io_base) < 0){
        return false;
    }

    // Read the Identify data 256-word
    uint16_t identify_data[256];
    for (int i = 0; i < 256; i++) {
        identify_data[i] = inw(io_base + ATA_REG_DATA);
    }

    // Total sectors: LBA28 (words 60-61), fall back to CHS geometry
    uint32_t total = identify_data[60] | ((uint32_t)identify_data[61] << 16);
    if (total == 0) {
        total = (uint32_t)identify_data[1] * identify_data[3] * identify_data[6];   // cylinders * heads * sectors-per-track
    }

    // Model string: words 27 - 46, byte-swapped
    for (int i = 0; i < 20 ; i++) {
        uint16_t w = identify_data[27 + i];
        out->model[2*i]     = (char) w >> 8;
        out->model[2*i + 1] = (char) w & 0xFF;
    }
    out->model[40] = '\0'; // Null-terminate the string

    out->present = true;
    out->io_base = io_base;
    out->ctrl_base = ctrl_base;
    out->is_slave = is_slave;
    out->total_sectors = total;

    return true;

}

void ata_init(void) {
    serial_write_string("\n=== ATA init ===\n");
    drive_count = 0;

    struct { uint16_t io, ctrl; bool slave; } candidates[4] = {
        { ATA_PRIMARY_IO, ATA_PRIMARY_CTRL, false },
        { ATA_PRIMARY_IO, ATA_PRIMARY_CTRL, true },
        { ATA_SECONDARY_IO, ATA_SECONDARY_CTRL, false },
        { ATA_SECONDARY_IO, ATA_SECONDARY_CTRL, true }
    };

    for (int i = 0; i < 4; i++) {
        struct ata_drive d;
        if (ata_detect(candidates[i].io, candidates[i].ctrl, candidates[i].slave, &d)) {
        if (d.present) {
            drives[drive_count] = d;
            }
        kprintf("ATA[%u]: %s %s, %u sectors (%u KB) - %s\n",
                (unsigned int)drive_count, candidates[i].io == ATA_PRIMARY_IO ? "primary" : "secondary",
                candidates[i].slave ? "slave" : "master",
                (unsigned int)d.total_sectors, (unsigned int)(d.total_sectors / 2), d.model);
        drive_count++;
        }

    }
    if (drive_count == 0) {
        kprintf("ATA: no drives detected.\n");   
    } else {
        kprintf("ATA: %u drive(s) detected.\n", (unsigned int)drive_count);
    }
    // Install IRQ handler, routing to IRQ14 via IOAPIC
    irq_register(ATA_PRIMARY_IRQ, ata_irq_handler);
    ioapic_route(ATA_PRIMARY_IRQ, ATA_PRIMARY_VECTOR, 0, false); // Route to CPU 0, unmasked
    kprintf("ATA: IRQ14 routed to vector %u\n", (unsigned int)ATA_PRIMARY_VECTOR);
}


uint32_t ata_drive_count(void) {
    return drive_count;
}


const struct ata_drive *ata_get_drive(uint32_t index) {
    if (index >= drive_count) return NULL;
    return &drives[index];
}


// Select drive and program LBA28 on given drive. drive 0 = master, 1 = slave
static void ata_setup_lba(const struct ata_drive *d, uint64_t lba, uint8_t count) {
    uint8_t drive_sel = (d->is_slave ? 0xF0 : 0xE0) | ((lba >> 24) & 0x0F);

    outb(d->io_base + ATA_REG_DRIVE, drive_sel);
    ata_io_wait(d->ctrl_base);

    outb(d->io_base + ATA_REG_SECCOUNT, count);
    outb(d->io_base + ATA_REG_LBA_LOW, (uint8_t)(lba & 0xFF));
    outb(d->io_base + ATA_REG_LBA_MID, (uint8_t)((lba >> 8) & 0xFF));
    outb(d->io_base + ATA_REG_LBA_HIGH, (uint8_t)((lba >> 16) & 0xFF));
}


int ata_read_sectors(uint32_t drive_index, uint64_t lba, uint8_t count, void *buffer) {
    if (drive_index >= drive_count) return -1;
    if (count == 0) return -1;

    const struct ata_drive *d = &drives[drive_index];
    uint16_t *buffer16 = (uint16_t *)buffer; // Cast buffer to uint16_t pointer

    if (ata_wait_not_busy(d->io_base) < 0) return -1;

    ata_irq_fired = false; // Clear the flag for the first sector's IRQ
    ata_setup_lba(d, lba, count); // Setup LBA and count
    outb(d->io_base + ATA_REG_COMMAND, ATA_CMD_READ_PIO); // Send Read command

    // Read each sector
    for (int i = 0; i < count; i++) {
        if (ata_wait_irq() < 0){
            kprintf("ATA: IRQ wait timeout at sctor %u\n", (unsigned int)i);
            return -1; // Wait for DRQ to set;
        }
        for (int j = 0; j < 256; j++) {
            buffer16[j] = inw(d->io_base + ATA_REG_DATA); // Read 256 bytes
        }
        buffer16 += 256; // Move buffer pointer forward
        // Clear the flag for the next sector's IRQ
        ata_irq_fired = false;
    }   
    return 0;
}

int ata_write_sectors(uint32_t drive_index, uint64_t lba, uint8_t count, const void *buffer) {
    if (drive_index >= drive_count) return -1;
    if (count == 0) return -1;

    const struct ata_drive *d = &drives[drive_index]; // Get drive pointer
    const uint16_t *buffer16 = (const uint16_t *)buffer; // Cast buffer to uint16_t pointer

    if (ata_wait_not_busy(d->io_base) < 0) return -1;

    ata_irq_fired = false;
    ata_setup_lba(d, lba, count);
    outb(d->io_base + ATA_REG_COMMAND, ATA_CMD_WRITE_PIO); // Send Write command

    // Write each sector
    for (int i = 0; i < count; i++) {
        // Wait for the drive to be ready to ACCEPT this sector's data
        // After WRITE command (and after each completed section), the drive sets DRQ when its buffer is ready
        // We poll DRQ here - the drive doesn't raise an irq to ask for data, only to confirm completion. 
        if (ata_wait_drq(d->io_base) < 0) {
            kprintf("ATA: write to DRQ failed at sector %u\n", (unsigned int)i); // Wait for DRQ to set
            return -1; // Wait for DRQ to set
        }

        // Write 256 bytes to the drive
        ata_irq_fired = false;
        for (int j = 0; j < 256; j++) {
            outw(d->io_base + ATA_REG_DATA, buffer16[j]); // Write 256 bytes
        }
        buffer16 += 256; // Move buffer pointer forward

        // Now that the drive write to the platter and raises IRQ 14 WHEN done.
        // Wait for the completion IRQ
        if (ata_wait_irq() < 0) {
            kprintf("ATA: IRQ wait timeout at sector %u\n", (unsigned int)i);
            return -1; // Wait for DRQ to set
        }
    }

    // Flush the cache after writing
    outb(d->io_base + ATA_REG_COMMAND, ATA_CMD_CACHE_FLUSH); // Send Cache flush command
    ata_wait_not_busy(d->io_base); // Wait for BSY to clear

    return 0;
}