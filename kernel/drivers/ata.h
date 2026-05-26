#ifndef __ATA_H__
#define __ATA_H__

#include "../include/types.h"
#include <stdint.h>


// Primary ATA channel I/O ports
#define ATA_PRIMARY_IO         0x1F0
#define ATA_PRIMARY_CTRL       0x3F6

// Secondary ATA channel I/O ports
#define ATA_SECONDARY_IO       0x170
#define ATA_SECONDARY_CTRL     0x376

// Register offsets from the channel I/O base
#define ATA_REG_DATA            0x00
#define ATA_REG_ERROR           0x01
#define ATA_REG_SECCOUNT        0x02
#define ATA_REG_LBA_LOW         0x03
#define ATA_REG_LBA_MID         0x04
#define ATA_REG_LBA_HIGH        0x05
#define ATA_REG_DRIVE           0x06
#define ATA_REG_STATUS          0x07   // READ
#define ATA_REG_COMMAND         0x07   // WRITE 

// Status register bits
#define ATA_STATUS_BSY          0x80    // Busy
#define ATA_STATUS_DRDY         0x40    // Drive ready
#define ATA_STATUS_DRQ          0x08    // Data request
#define ATA_STATUS_ERR          0x01    // Error

// Command register bits
#define ATA_CMD_READ_PIO        0x20    // Read
#define ATA_CMD_WRITE_PIO       0x30    // Write
#define ATA_CMD_IDENTIFY        0xEC    // Identify drive
#define ATA_CMD_CACHE_FLUSH     0xE7    // Flush cache

#define ATA_SECTOR_SIZE         512

#define ATA_MAX_DRIVES          4

// a detectec ATA drive
struct ata_drive {
    bool     present;
    uint16_t io_base;  // Channel I/O base
    uint16_t ctrl_base;  // Control I/O base
    bool     is_slave;  // Slave drive
    uint32_t total_sectors;  // Total number of sectors
    char     model[41];  // 40 char + null name
};

// Initialize: detect drive via Identify
void ata_init();

// How many drives are detected
uint32_t ata_drive_count(void);

// Get a drive descriptor by index (0 ...count-1)
const struct ata_drive *ata_get_drive(uint32_t index);

// Read `count` sectors starting from `lba` into `buffer`. Return 0 on success, -1 on error.
int ata_read_sectors(uint32_t drive_index, uint64_t lba, uint8_t count, void *buffer);

// Write `count` sectors starting from `lba` from `buffer`. Return 0 on success, -1 on error.
int ata_write_sectors(uint32_t drive_index, uint64_t lba, uint8_t count, const void *buffer);

int ata_read_dma(uint32_t drive_index, uint64_t lba, uint8_t count, void *buffer);

#endif  // __ATA_H__
