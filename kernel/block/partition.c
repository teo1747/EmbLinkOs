#include "block/partition.h"
#include "block/block.h"
#include "include/kprintf.h"
#include "include/errno.h"
#include "include/kstring.h"

#include <stdint.h>

// ---------------------------------------------------------------------------
// MBR (DOS) partition table layout.
//
//   sector 0 (512 bytes):
//     [0   .. 445]  bootstrap code
//     [446 .. 509]  4 partition entries, 16 bytes each
//     [510 .. 511]  boot signature 0x55 0xAA
//
//   16-byte partition entry:
//     +0   boot flag (0x80 = active, 0x00 = inactive)
//     +1   starting CHS (ignored — we use LBA)
//     +4   partition type (0x00 = empty / unused)
//     +5   ending CHS (ignored)
//     +8   starting LBA          (uint32, little-endian)
//     +12  number of sectors     (uint32, little-endian)
// ---------------------------------------------------------------------------

#define MBR_PART_TABLE_OFFSET   446
#define MBR_PART_ENTRY_SIZE     16
#define MBR_PART_COUNT          4
#define MBR_SIG_OFFSET          510
#define MBR_SIG_0               0x55
#define MBR_SIG_1               0xAA

#define MBR_TYPE_EMPTY          0x00
#define MBR_TYPE_GPT_PROTECTIVE 0xEE   // whole disk is GPT, not MBR
#define MBR_TYPE_EXTENDED       0x05   // CHS extended (logical partitions)
#define MBR_TYPE_EXTENDED_LBA   0x0F   // LBA  extended

// A partition device: an embk_block_device that forwards to its parent disk,
// shifted by start_lba. dev is the FIRST member so a (struct embk_partition *)
// and a (struct embk_block_device *) alias the same address — driver_data also
// points back here for clarity.
struct embk_partition {
    struct embk_block_device  dev;       // the child block device (registered)
    struct embk_block_device *parent;    // underlying whole disk
    uint64_t                  start_lba;  // offset of this partition in parent
    bool                      in_use;
};

// Pool of partition descriptors. The block registry caps total devices at
// BLOCK_MAX_DEVICES, so this is an ample upper bound.
static struct embk_partition partitions[BLOCK_MAX_DEVICES];

static struct embk_partition *partition_alloc(void) {
    for (uint32_t i = 0; i < BLOCK_MAX_DEVICES; i++) {
        if (!partitions[i].in_use) {
            return &partitions[i];
        }
    }
    return NULL;
}

// Read a little-endian uint32 out of a byte buffer.
static uint32_t rd_le32(const uint8_t *p) {
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

// --- partition block-device ops: offset into the parent, then delegate -----

static int part_read(struct embk_block_device *dev,
                     uint64_t lba, uint32_t count, void *buffer) {
    struct embk_partition *p = (struct embk_partition *)dev->driver_data;
    if (lba + count > dev->block_count) return -EMBK_ERANGE;
    // Buffer DMA-safety was already validated by embk_block_read against this
    // partition's constraints (copied from the parent), so call the parent's
    // raw op directly to avoid a redundant second bounce.
    return p->parent->read(p->parent, p->start_lba + lba, count, buffer);
}

static int part_write(struct embk_block_device *dev,
                      uint64_t lba, uint32_t count, const void *buffer) {
    struct embk_partition *p = (struct embk_partition *)dev->driver_data;
    if (!p->parent->write) return -EMBK_EROFS;
    if (lba + count > dev->block_count) return -EMBK_ERANGE;
    return p->parent->write(p->parent, p->start_lba + lba, count, buffer);
}

static int part_flush(struct embk_block_device *dev) {
    struct embk_partition *p = (struct embk_partition *)dev->driver_data;
    // A partition has no cache of its own; the cache lives on the disk. Forward
    // so a filesystem's write barrier reaches the platter.
    return embk_block_flush(p->parent);
}

// Compose "<disk>N" (e.g. "sda" + 2 -> "sda2") into out. index is 1-based.
static void make_part_name(char *out, const char *disk, uint32_t index) {
    size_t n = strlen(disk);
    if (n >= BLOCK_NAME_LEN - 2) n = BLOCK_NAME_LEN - 2;  // leave room for digit + NUL
    for (size_t i = 0; i < n; i++) out[i] = disk[i];
    // index is 1..4 for primary MBR partitions — a single digit suffices.
    out[n]     = (char)('0' + (index % 10));
    out[n + 1] = '\0';
}

static int register_one_partition(struct embk_block_device *disk,
                                   uint32_t index,
                                   uint8_t type,
                                   uint64_t start_lba,
                                   uint64_t sectors) {
    // Reject entries that fall outside the parent disk — a corrupt or hostile
    // MBR must not let a filesystem address sectors beyond the device.
    if (start_lba >= disk->block_count ||
        sectors == 0 ||
        start_lba + sectors > disk->block_count) {
        kprintf("part: %s entry %u out of range (start=%u sectors=%u), skipping\n",
                disk->name, (unsigned int)index,
                (unsigned int)start_lba, (unsigned int)sectors);
        return -EMBK_ERANGE;
    }

    struct embk_partition *p = partition_alloc();
    if (!p) {
        kprintf("part: no free partition slots for %s entry %u\n",
                disk->name, (unsigned int)index);
        return -EMBK_ENOMEM;
    }

    memset(&p->dev, 0, sizeof(p->dev));
    p->parent    = disk;
    p->start_lba = start_lba;

    make_part_name(p->dev.name, disk->name, index);
    p->dev.block_count = sectors;
    p->dev.block_size  = disk->block_size;
    p->dev.read        = part_read;
    p->dev.write       = disk->write ? part_write : NULL;
    p->dev.flush       = part_flush;
    p->dev.driver_data = p;
    // Inherit the parent's DMA constraints so the block layer bounces (when
    // needed) at the partition level using the same rules as the disk.
    p->dev.dma_max_phys       = disk->dma_max_phys;
    p->dev.needs_kernel_range = disk->needs_kernel_range;

    int rc = embk_block_register(&p->dev);
    if (rc != EMBK_OK) {
        return rc;
    }
    p->in_use = true;

    kprintf("part: %s type 0x%X  start=%u  sectors=%u (%u MB)\n",
            p->dev.name, (unsigned int)type,
            (unsigned int)start_lba, (unsigned int)sectors,
            (unsigned int)(((uint64_t)sectors * disk->block_size) / (1024 * 1024)));
    return EMBK_OK;
}

int embk_partition_scan(struct embk_block_device *disk) {
    if (!disk || !disk->read) return -EMBK_ENODEV;
    if (disk->block_size != 512) {
        // MBR is defined for 512-byte sectors; skip anything else for now.
        return 0;
    }

    uint8_t sector[512];
    int rc = embk_block_read(disk, 0, 1, sector);
    if (rc != EMBK_OK) {
        kprintf("part: %s: failed to read MBR (%d)\n", disk->name, rc);
        return rc;
    }

    // Validate the boot signature before trusting the table.
    if (sector[MBR_SIG_OFFSET] != MBR_SIG_0 ||
        sector[MBR_SIG_OFFSET + 1] != MBR_SIG_1) {
        // No partition table — the whole disk is likely a bare filesystem.
        return 0;
    }

    // A GPT disk carries a single protective MBR entry of type 0xEE. We don't
    // parse GPT yet; flag it so the disk isn't silently treated as unpartitioned.
    {
        const uint8_t *e0 = sector + MBR_PART_TABLE_OFFSET;
        if (e0[4] == MBR_TYPE_GPT_PROTECTIVE) {
            kprintf("part: %s has a GPT (protective MBR) — GPT parsing not yet supported\n",
                    disk->name);
            return 0;
        }
    }

    int registered = 0;
    for (uint32_t i = 0; i < MBR_PART_COUNT; i++) {
        const uint8_t *e = sector + MBR_PART_TABLE_OFFSET + i * MBR_PART_ENTRY_SIZE;
        uint8_t  type      = e[4];
        uint32_t start_lba = rd_le32(e + 8);
        uint32_t sectors   = rd_le32(e + 12);

        if (type == MBR_TYPE_EMPTY) {
            continue;
        }
        if (type == MBR_TYPE_EXTENDED || type == MBR_TYPE_EXTENDED_LBA) {
            // Logical partitions live in a linked list inside the extended
            // partition; not walked yet. Skip with a note.
            kprintf("part: %s entry %u is an extended partition — logical "
                    "partitions not yet supported\n",
                    disk->name, (unsigned int)(i + 1));
            continue;
        }

        if (register_one_partition(disk, i + 1, type, start_lba, sectors) == EMBK_OK) {
            registered++;
        }
    }

    return registered;
}

void embk_partition_scan_all(void) {
    // Snapshot the count first: embk_partition_scan appends partition devices to
    // the registry, and we must not scan those (a partition has no sub-MBR).
    uint32_t disk_count = embk_block_count();

    kprintf("\n=== Partition scan ===\n");
    for (uint32_t i = 0; i < disk_count; i++) {
        struct embk_block_device *disk = embk_block_get(i);
        if (!disk) continue;
        int n = embk_partition_scan(disk);
        if (n == 0) {
            kprintf("part: %s has no MBR partitions\n", disk->name);
        }
    }
}
