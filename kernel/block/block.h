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

// Drain the device's write-back cache so every previously-completed write is
// durable on the medium (ATA FLUSH CACHE, or an FUA equivalent). Optional: a
// driver leaves this NULL if it has no cache to flush.
typedef int (*embk_block_flush_fn)(struct embk_block_device *dev);


// A generic block device: anything that reads/writes fixed-size blocks
// addressed by block number (LBA). Driveers fill this in and register it.
struct embk_block_device {
    char     name[BLOCK_NAME_LEN] ; // Name of the device (e.g., "sda", "ata0", etc.)
    uint64_t block_count; // Total number of blocks on the device
    uint32_t block_size; // Size of a single block in bytes 512 for our disks

    // Function pointers for read/write operations
    embk_block_read_fn read;
    embk_block_write_fn write;
    embk_block_flush_fn flush;   // optional: make prior writes durable; NULL = no cache

    // Driver-specific data (e.g., ATA drive info, etc.)
    void *driver_data;

    // DMA constraints (filled by the driver at registration)
    uint64_t dma_max_phys;   // highest physical address the controller can DMA to
                             // (e.g. 0xFFFFFFFF for 32-bit ATA, UINT64_MAX for 64-bit AHCI)
    bool needs_kernel_range;  // true if buffer must be KV2P-able (both, for now)
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

// Force all previously-completed writes to durable media. Filesystems call this
// as a WRITE BARRIER: e.g. EMBKFS flushes the new metadata tree before writing
// the superblock that points at it, so a power loss can't expose a committed
// superblock whose targets never reached the platter (a drive's write cache can
// make writes durable out of the order they were issued). Returns EMBK_OK if the
// device exposes no flush op (it is then assumed to have no write-back cache).
int embk_block_flush(struct embk_block_device *dev);



#endif /* _BLOCK_H_ */