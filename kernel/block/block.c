#include "block.h"
#include "../include/kprintf.h"
#include "../include/errno.h"
#include "../include/kstring.h"
#include "../drivers/serial.h"

#include <stdint.h>


// The registry: pointers to all registered block devices.
static struct embk_block_device *devices[BLOCK_MAX_DEVICES];
static uint32_t device_count = 0;

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

int embk_block_read(struct embk_block_device *dev,
                           uint64_t lba, uint32_t count, void *buffer) {
    if (!dev || !dev->read) {
        return -EMBK_EINVAL; // Invalid device or missing read function
    }
    
    if (count == 0) {
        return -EMBK_EINVAL; // Nothing to read, but not an error
    }
    
    // Bounds - check: don't read past the end of the device.
    if (lba + count > dev->block_count) {
        kprintf("block: %s read out of range (lba=%llu, count=%u, max=%llu)\n",
                dev->name, (unsigned long long)lba, (unsigned int)count,
                (unsigned long long)dev->block_count);

        return -EMBK_ERANGE; // Out of range
    }
    return dev->read(dev, lba, count, buffer);
}

int embk_block_write(struct embk_block_device *dev,
                            uint64_t lba, uint32_t count, const void *buffer) {
    if (!dev || !dev->write) {
        return -EMBK_EINVAL; // Invalid device or missing write function
    }
    
    if (count == 0) {
        return -EMBK_EINVAL; // Nothing to write, but not an error
    }
    
    // Bounds - check: don't write past the end of the device.
    if (lba + count > dev->block_count) {
        kprintf("block: %s write out of range (lba=%llu, count=%u, max=%llu)\n",
                dev->name, (unsigned long long)lba, (unsigned int)count,
                (unsigned long long)dev->block_count);

        return -EMBK_ERANGE; // Out of range
    }
    return dev->write(dev, lba, count, buffer);
}