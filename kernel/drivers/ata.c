#include "ata.h"
#include "../include/io.h"
#include "../include/kprintf.h"
#include "../drivers/serial.h"

#include <stdint.h>


static bool driver_present = false;

// 400ns delY: READ ALTERNATE STATUS 4 TIMESS (~100ns each)

static void ata_io_wait(void) {
    inb(ATA_PRIMARY_ALT_STATUS);
    inb(ATA_PRIMARY_ALT_STATUS);
    inb(ATA_PRIMARY_ALT_STATUS); // 400ns delay
    inb(ATA_PRIMARY_ALT_STATUS);
}


// SPIN until BSY clears. Returns 0 on success, -1 on error.
static int ata_wait_not_busy(void) { 
    // Generous timeout to aviod hanging forever on bad hardware
    for (int i = 0; i < 100000; i++) { 
        uint8_t status = inb(ATA_PRIMARY_STATUS);
        if (!(status & ATA_STATUS_BSY)) { 
            return 0; 
        }
        
    }
    return -1;
}


// Spin until DRQ sets (data ready). Returns 0 on success, -1 on error.
static int ata_wait_drq(void) {
    for (int i = 0; i < 100000; i++) {
        uint8_t status = inb(ATA_PRIMARY_STATUS);
        if (status & ATA_STATUS_ERR) {
            return -1;
        }
        if (!(status & ATA_STATUS_BSY) && (status & ATA_STATUS_DRQ)) {
            return 0;
        }
  
    }
  return -1;
}

// Select drive + program LBA. drive 0 = master, 1 = slave
static void ata_setup_lba(uint64_t lba, uint8_t count) {
    // Drive/head: 0xEO = master LBA mode. Top 4 LBA bits in low nibble, 0x01 = slave
    outb(ATA_PRIMARY_DRIVE, 0XE0 | ((lba >> 24) & 0x0F));
    ata_io_wait();
    outb(ATA_PRIMARY_SECCOUNT, count);
    outb(ATA_PRIMARY_LBA_LO, (uint8_t)(lba & 0xFF));
    outb(ATA_PRIMARY_LBA_MID, (uint8_t)((lba >> 8) & 0xFF));
    outb(ATA_PRIMARY_LBA_HI, (uint8_t)((lba >> 16) & 0xFF));
}

void ata_init(void) {
    serial_write_string("\n=== ATA init ===\n");

    // select master drive
    outb(ATA_PRIMARY_DRIVE, 0xE0);
    ata_io_wait();

    // Zero out the LBA/count registers, then send Identify command
    outb(ATA_PRIMARY_SECCOUNT, 0);
    outb(ATA_PRIMARY_LBA_LO, 0);
    outb(ATA_PRIMARY_LBA_MID, 0);
    outb(ATA_PRIMARY_LBA_HI, 0);
    outb(ATA_PRIMARY_COMMAND, ATA_CMD_IDENTIFY);

    // check if ATA device is present
    uint8_t status = inb(ATA_PRIMARY_STATUS);
    if ( status == 0 ) {
        kprintf("ATA device no Primary master detected.\n");
        driver_present = false;
        return;
    }

    // Wait for BSY to clear
    if (ata_wait_not_busy() < 0) {
        kprintf("ATA: Timeout waiting for BSY to clear.\n");
        return;
    }

    // check LBA mid/hi if nonzero, its not an ATA drive (COULD be ATAPI)
    if (inb(ATA_PRIMARY_LBA_MID) != 0 || inb(ATA_PRIMARY_LBA_HI) != 0) {
        kprintf("ATA: not an ATA drive (maybe ATAPI)\n");
        return;
    }

    // Wait for DRQ to set
    if (ata_wait_drq() < 0) {
        kprintf("ATA: Timeout waiting for DRQ to set.\n");
        return;
    }

    // Read the Identify data 256-word
    uint8_t identify_data[256];
    for (int i = 0; i < 256; i++) {
        identify_data[i] = inw(ATA_PRIMARY_DATA);
    }

    // Word 60-61 = LBA28 total sectors. If zero (tiny/CHS-only disk),
    // fall back to CHS geometry: cylinders * heads * sectors-per-track.
    uint32_t total_sectors = identify_data[60] | ((uint32_t)identify_data[61] << 16);

    if (total_sectors == 0) {
        uint32_t cylinders = identify_data[1];
        uint32_t heads     = identify_data[3];
        uint32_t spt       = identify_data[6];
        total_sectors = cylinders * heads * spt;
        kprintf("ATA: using CHS geometry (%u/%u/%u)\n",
                (unsigned int)cylinders, (unsigned int)heads, (unsigned int)spt);
    }

    driver_present = true;
    kprintf("ATA: drive detected, %u sectors (%u KB)\n",
            (unsigned int)total_sectors,
            (unsigned int)(total_sectors / 2));  // sectors * 512 / 1024
}

int ata_read_sectors(uint64_t lba, uint8_t count, void *buffer) {
    if (!driver_present) return -1;
    if (count == 0) return -1;

    uint16_t *buffer16 = (uint16_t *)buffer; // Cast buffer to uint16_t pointer

    if (ata_wait_not_busy() < 0) return -1;

    ata_setup_lba(lba, count); // Setup LBA and count

    outb(ATA_PRIMARY_COMMAND, ATA_CMD_READ_PIO); // Send Read command

    // Read each sector
    for (int i = 0; i < count; i++) {
        if (ata_wait_drq() < 0) {
            kprintf("ATA: read DRQ failed at sector %u\n", (unsigned int)i);
            return -1; // Wait for DRQ to set
        }
        for (int j = 0; j < 256; j++) {
            buffer16[j] = inw(ATA_PRIMARY_DATA); // Read 256 bytes
        }
        buffer16 += 256; // Move buffer pointer forward
    }
    return 0;
}

int ata_write_sectors(uint64_t lba, uint8_t count, const void *buffer) {
    if (!driver_present) return -1;
    if (count == 0) return -1;

    const uint16_t *buffer16 = (const uint16_t *)buffer; // Cast buffer to uint16_t pointer

    if (ata_wait_not_busy() < 0) return -1;

    ata_setup_lba(lba, count);
    outb(ATA_PRIMARY_COMMAND, ATA_CMD_WRITE_PIO); // Send Write command

    // Write each sector
    for (int i = 0; i < count; i++) {
        if (ata_wait_drq() < 0) {
            return -1; // Wait for DRQ to set
        }
        for (int j = 0; j < 256; j++) {
            outw(ATA_PRIMARY_DATA, buffer16[j]); // Write 256 bytes
        }
        buffer16 += 256; // Move buffer pointer forward
    }

    // Flush the cache after writing
    outb(ATA_PRIMARY_COMMAND, 0xE7); // Send Flush command
    ata_wait_not_busy(); // Wait for BSY to clear

    return 0;
}