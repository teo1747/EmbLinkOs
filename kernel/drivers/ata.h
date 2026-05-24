#ifndef __ATA_H__
#define __ATA_H__

#include "../include/types.h"
#include <stdint.h>


// Primary ATA channel I/O ports
#define ATA_PRIMARY_DATA        0x1F0
#define ATA_PRIMARY_ERROR       0x1F1
#define ATA_PRIMARY_SECCOUNT    0X1F2
#define ATA_PRIMARY_LBA_LO      0x1F3
#define ATA_PRIMARY_LBA_MID     0x1F4
#define ATA_PRIMARY_LBA_HI      0x1F5
#define ATA_PRIMARY_DRIVE       0x1F6
#define ATA_PRIMARY_STATUS      0x1F7   // READ
#define ATA_PRIMARY_COMMAND     0x1F7   // WRITE 
#define ATA_PRIMARY_ALT_STATUS  0x3F6   // READ (control port)

// Status register bits
#define ATA_STATUS_BSY          0x80    // Busy
#define ATA_STATUS_DRDY         0x40    // Drive ready
#define ATA_STATUS_DRQ          0x08    // Data request
#define ATA_STATUS_ERR          0x01    // Error

// Command register bits
#define ATA_CMD_READ_PIO        0x20    // Read
#define ATA_CMD_WRITE_PIO       0x30    // Write
#define ATA_CMD_IDENTIFY        0xEC    // Identify drive

#define ATA_SECTOR_SIZE         512


// Initialize: detect drive via Identify
void ata_init();

// Read `count` sectors starting from `lba` into `buffer`. Return 0 on success, -1 on error.
int ata_read_sectors(uint64_t lba, uint8_t count, void *buffer);

// Write `count` sectors starting from `lba` from `buffer`. Return 0 on success, -1 on error.
int ata_write_sectors(uint64_t lba, uint8_t count, const void *buffer);


#endif  // __ATA_H__
