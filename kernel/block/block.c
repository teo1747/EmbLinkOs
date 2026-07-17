#include "block/block.h"
#include "include/kprintf.h"
#include "include/errno.h"
#include "include/kstring.h"
#include "drivers/char/serial.h"
#include "include/kstring.h"   // for memcpy
#include "mm/pmm.h"            // for KV2P, KERNEL_VIRTUAL_BASE
#include "drivers/timer/hpet.h" // blkstat time-in-device attribution

#include <stdint.h>


// The registry: pointers to all registered block devices.
static struct embk_block_device *devices[BLOCK_MAX_DEVICES];
static uint32_t device_count = 0;

// Whole-disk letter counter. Only auto-named disks consume a letter (sda, sdb,
// ...). Partitions name themselves (sda1, sda2, ...) and must NOT advance this,
// or the next disk would skip a letter.
static uint32_t disk_letters = 0;


// One shared bounce buffer in kernel BSS (low physical, KV2P-able, < 4GB).
// Sized to the max single transfer the adapters issue (64 sectors = 32KB).
static uint8_t block_bounce[64 * 512] __attribute__((aligned(16)));

// Request counters (see block.h). Counts requests that actually reach a
// driver -- the bounce path issues several per call, so this is incremented at the
// dev->read sites, NOT at entry to embk_block_read.
static struct embk_blkstat blkstat;

void embk_blkstat_reset(void) { blkstat.reads = 0; blkstat.read_blocks = 0; blkstat.read_us = 0; }
void embk_blkstat_get(struct embk_blkstat *out) { if (out) *out = blkstat; }

// Microseconds, straight off the HPET. Only used to attribute wall time to the
// device, so a 0 on a machine without an HPET just makes the attribution empty
// rather than wrong.
static uint64_t blk_now_us(void) {
    if (!hpet_available()) return 0;
    uint64_t pf = hpet_period_fs();               // femtoseconds per tick
    if (!pf) return 0;
    uint64_t tpus = 1000000000ULL / pf;           // ticks per microsecond
    if (tpus == 0) tpus = 1;
    return hpet_read_counter() / tpus;
}

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
    
    // Auto-name whole disks sda, sdb, sdc ... A caller that has already filled
    // in dev->name (e.g. the partition layer registering "sda1") keeps its name.
    if (dev->name[0] == '\0') {
        dev->name[0] =  's';
        dev->name[1] =  'd';
        dev->name[2] = (char)('a' + disk_letters);
        dev->name[3] = '\0'; // Null-terminate the string
        disk_letters++;
    }

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
        blkstat.reads++; blkstat.read_blocks += count;
        uint64_t t0 = blk_now_us();
        int rc = dev->read(dev, lba, count, buffer);
        blkstat.read_us += blk_now_us() - t0;
        return rc;
    }

    // Bounce path: read in bounce-sized chunks, copy out.
    uint32_t max_blocks = sizeof(block_bounce) / dev->block_size;
    uint8_t *dst = (uint8_t *)buffer;
    while (count > 0) {
        uint32_t chunk = (count > max_blocks) ? max_blocks : count;
        blkstat.reads++; blkstat.read_blocks += chunk;
        uint64_t t0 = blk_now_us();
        int rc = dev->read(dev, lba, chunk, block_bounce);
        blkstat.read_us += blk_now_us() - t0;
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