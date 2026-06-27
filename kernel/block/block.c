#include "block.h"
#include "../include/kprintf.h"
#include "../include/errno.h"
#include "../include/kstring.h"
#include "../drivers/serial.h"
#include "../include/kstring.h"   // for memcpy
#include "../mm/pmm.h"            // for KV2P, KERNEL_VIRTUAL_BASE

#include <stdint.h>


// The registry: pointers to all registered block devices.
static struct embk_block_device *devices[BLOCK_MAX_DEVICES];
static uint32_t device_count = 0;


// One shared bounce buffer in kernel BSS (low physical, KV2P-able, < 4GB).
// Sized to the max single transfer the adapters issue (64 sectors = 32KB).
static uint8_t block_bounce[64 * 512] __attribute__((aligned(16)));

// Does `buf` satisfy `dev`'s DMA constraints?
static bool buffer_dma_ok(struct embk_block_device *dev, const void *buf) {
    uint64_t v = (uint64_t)buf;

    // Must be in the kernel range so the driver's KV2P is valid.
    if (dev->needs_kernel_range && v < KERNEL_VIRTUAL_BASE) {
        return false;
    }
    // Physical address must be within the controller's reach.
    uint64_t phys = KV2P(v);
    if (phys > dev->dma_max_phys) {
        return false;
    }
    return true;
}

int embk_block_register(struct embk_block_device *dev) {
    if (device_count >= BLOCK_MAX_DEVICES) {
        kprintf("block: registry full, cannot register devices\n");
        return -EMBK_ENOMEM; // No space left for new device
    }
    if (!dev || !dev->read) {
        kprintf("block: refusing to register device with no read fn\n");
        return -EMBK_EINVAL; // Invalid device or missing read function
    }
    
    // Assign the next name : sda, sdb, sdc ...
    dev->name[0] =  's';
    dev->name[1] =  'd';
    dev->name[2] = (char)('a' + device_count);
    dev->name[3] = '\0'; // Null-terminate the string

    devices[device_count++] = dev;

    kprintf("block: registered %s (%u blocks x %u bytes = %u KB)\n",
            dev->name, (unsigned int)dev->block_count, (unsigned int)dev->block_size,
            (unsigned int)((dev->block_count * dev->block_size) / 1024));
    return EMBK_OK ; // Success
}

uint32_t embk_block_count(void) {
    return device_count;
}

struct embk_block_device *embk_block_get(uint32_t index) {
    if (index >= device_count) {
        return (struct embk_block_device *)NULL; // Out of bounds
    }
    return devices[index];
}

struct embk_block_device *embk_block_get_by_name(const char *name) {
    for (uint32_t i = 0; i < device_count; i++) {
        if (strcmp(devices[i]->name, name) == 0) {
            return devices[i];
        }
    }
    return (struct embk_block_device *)NULL; // Not found
}

// Read from a block device. Uses the same bounce buffer as write.
// If buffer is not DMA-safe, copies to bounce buffer first. 

int embk_block_read(struct embk_block_device *dev,
                    uint64_t lba, uint32_t count, void *buffer) {
    if (!dev || !dev->read) return -EMBK_ENODEV;
    if (!buffer)            return -EMBK_EFAULT;
    if (count == 0)         return EMBK_OK;
    if (lba + count > dev->block_count) {
        kprintf("block: %s read out of range\n", dev->name);
        return -EMBK_ERANGE;
    }

    // Fast path: buffer already DMA-safe for this device.
    if (buffer_dma_ok(dev, buffer)) {
        return dev->read(dev, lba, count, buffer);
    }

    // Bounce path: read in bounce-sized chunks, copy out.
    uint32_t max_blocks = sizeof(block_bounce) / dev->block_size;
    uint8_t *dst = (uint8_t *)buffer;
    while (count > 0) {
        uint32_t chunk = (count > max_blocks) ? max_blocks : count;
        int rc = dev->read(dev, lba, chunk, block_bounce);
        if (rc != EMBK_OK) return rc;
        memcpy(dst, block_bounce, (size_t)chunk * (size_t)dev->block_size);
        lba   += chunk;
        count -= chunk;
        dst   += chunk * dev->block_size;
    }
    return EMBK_OK;
}

// Write to a block device. Uses the same bounce buffer as read.
// If buffer is not DMA-safe, copies to bounce buffer first.
int embk_block_write(struct embk_block_device *dev,
                     uint64_t lba, uint32_t count, const void *buffer) {
    if (!dev || !dev->write) return -EMBK_ENODEV;
    if (!buffer)             return -EMBK_EFAULT;
    if (count == 0)          return EMBK_OK;
    if (lba + count > dev->block_count) {
        kprintf("block: %s write out of range\n", dev->name);
        return -EMBK_ERANGE;
    }

    if (buffer_dma_ok(dev, buffer)) {
        return dev->write(dev, lba, count, buffer);
    }

    uint32_t max_blocks = sizeof(block_bounce) / dev->block_size;
    const uint8_t *src = (const uint8_t *)buffer;
    while (count > 0) {
        uint32_t chunk = (count > max_blocks) ? max_blocks : count;
        memcpy(block_bounce, src, (size_t)chunk * (size_t)dev->block_size);
        int rc = dev->write(dev, lba, chunk, block_bounce);
        if (rc != EMBK_OK) return rc;
        lba   += chunk;
        count -= chunk;
        src   += chunk * dev->block_size;
    }
    return EMBK_OK;
}

// Drain the device's write-back cache. A device with no flush op is assumed to
// have no cache to drain, so the barrier is a no-op (success) there.
int embk_block_flush(struct embk_block_device *dev) {
    if (!dev)        return -EMBK_ENODEV;
    if (!dev->flush) return EMBK_OK;
    return dev->flush(dev);
}