#ifndef _BLOCK_H_
#define _BLOCK_H_


#include <stdint.h>
#include "../include/types.h"


#define BLOCK_NAME_LEN    16 // Max length of a block device name (including null terminator)
#define BLOCK_MAX_DEVICES 16 // Max number of block devices that can be registered

// Forward delaration so the function-pointer types can reference the struct.
struct embk_block_device;


// Operation fonction pointer types.
// Each driver implement these and stores them in its block_device.
typedef int (*embk_block_read_fn)(struct embk_block_device *dev,
                                        uint64_t lba, uint32_t count, void *buffer);

typedef int (*embk_block_write_fn)(struct embk_block_device *dev,
                                         uint64_t lba, uint32_t count, const void *buffer);


// A generic block device: anything that reads/writes fixed-size blocks
// addressed by block number (LBA). Driveers fill this in and register it.
struct embk_block_device {
    char     name[BLOCK_NAME_LEN] ; // Name of the device (e.g., "sda", "ata0", etc.)
    uint64_t block_count; // Total number of blocks on the device
    uint32_t block_size; // Size of a single block in bytes 512 for our disks

    // Function pointers for read/write operations
    embk_block_read_fn read;
    embk_block_write_fn write;

    // Driver-specific data (e.g., ATA drive info, etc.)
    void *driver_data;
};


// Register a block device with the layer. The layer assigns the next
// available name (sda, sdb, etc.) into dev->name. Returns 0 on success,
// negative error code on failure.
// The driver owns the struct's memory; the layer stores a pointer to it.
int embk_block_register(struct embk_block_device *dev);

// How many block devices are reistered?.
uint32_t embk_block_count(void);

// Get a registered device by index (0..count-1), or NULL.
struct embk_block_device *embk_block_get(uint32_t index);

// Find a registered device by name ("sda"), or NULL.
struct embk_block_device *embk_block_get_by_name(const char *name);


// Convenience wrappers with bounds-checking. Higher layers (Filesystem)
// Call these instead of dev->reqd/write directly - they validate the LBA range
// against block_count before dispatching to driver.
int embk_block_read(struct embk_block_device *dev,
                           uint64_t lba, uint32_t count, void *buffer);

int embk_block_write(struct embk_block_device *dev,
                            uint64_t lba, uint32_t count, const void *buffer);



#endif /* _BLOCK_H_ */