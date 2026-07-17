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

static uint64_t rd_le64(const uint8_t *p) {
    return (uint64_t)rd_le32(p) | ((uint64_t)rd_le32(p + 4) << 32);
}

/* CRC32 (IEEE 802.3, poly 0xEDB88320 reflected) -- what GPT specifies.
 *
 * ⚠️ NOT the same as kernel/fs/embkfs/crc32c.c. That is CRC32**C**
 * (Castagnoli, 0x82F63B78) -- a DIFFERENT polynomial that produces different
 * values for the same bytes. Reusing it here because "we already have a crc32"
 * would reject every real GPT on earth. They are not interchangeable.
 *
 * Bitwise, not table-driven: this runs a few times at boot over <= 16 KB, and
 * a 1 KB table to save microseconds nobody will ever measure is a bad trade. */
static uint32_t crc32_ieee(const uint8_t *p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(c & 1)));
    }
    return ~c;
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
    if (n >= BLOCK_NAME_LEN - 3) n = BLOCK_NAME_LEN - 3;  // room for 2 digits + NUL
    for (size_t i = 0; i < n; i++) out[i] = disk[i];
    // TWO digits now, and not for tidiness: MBR primaries are 1..4, but logical
    // partitions start at 5 and GPT allows 128. A single digit made "sda12"
    // collide with "sda2" -- two devices, one name, and the block registry would
    // hand out whichever it found first.
    if (index >= 10) {
        out[n]     = (char)('0' + (index / 10) % 10);
        out[n + 1] = (char)('0' + (index % 10));
        out[n + 2] = '\0';
    } else {
        out[n]     = (char)('0' + index);
        out[n + 1] = '\0';
    }
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

/* --- GPT ------------------------------------------------------------------
 * The header lives at LBA 1, behind a protective MBR (type 0xEE at LBA 0) whose
 * only job is to stop MBR-only tools from seeing free space and eating the disk.
 *
 * We VALIDATE the header CRC before trusting a single field. That is not
 * ceremony: every number we are about to act on -- where the entries live, how
 * many, how big -- comes out of this block, and a corrupt one would have us
 * register partitions over arbitrary sectors. Fail closed. */
static int gpt_scan(struct embk_block_device *disk) {
    uint8_t hdr[512];
    if (embk_block_read(disk, 1, 1, hdr) != EMBK_OK) {
        kprintf("part: %s: GPT header (LBA 1) unreadable\n", disk->name);
        return 0;
    }
    static const char sig[8] = { 'E','F','I',' ','P','A','R','T' };
    for (int i = 0; i < 8; i++) if (hdr[i] != (uint8_t)sig[i]) {
        kprintf("part: %s: protective MBR but no GPT header signature\n", disk->name);
        return 0;
    }

    uint32_t hsize = rd_le32(hdr + 12);
    if (hsize < 92 || hsize > 512) {
        kprintf("part: %s: GPT header_size %u implausible\n", disk->name, (unsigned)hsize);
        return 0;
    }
    /* The CRC is computed with its OWN field zeroed -- it cannot cover itself. */
    uint32_t stored = rd_le32(hdr + 16);
    uint8_t tmp[512];
    for (uint32_t i = 0; i < hsize; i++) tmp[i] = hdr[i];
    tmp[16] = tmp[17] = tmp[18] = tmp[19] = 0;
    uint32_t calc = crc32_ieee(tmp, hsize);
    if (calc != stored) {
        kprintf("part: %s: GPT header CRC32 mismatch (got 0x%X want 0x%X) — refusing\n",
                disk->name, (unsigned)calc, (unsigned)stored);
        return 0;   /* the backup header at the last LBA could be tried; not yet */
    }

    uint64_t ent_lba = rd_le64(hdr + 72);
    uint32_t n_ent   = rd_le32(hdr + 80);
    uint32_t ent_sz  = rd_le32(hdr + 84);
    if (ent_sz < 128 || ent_sz > 512 || (512 % ent_sz) != 0) {
        kprintf("part: %s: GPT entry size %u unsupported\n", disk->name, (unsigned)ent_sz);
        return 0;
    }
    if (n_ent > 128) n_ent = 128;        /* the spec's usual max; bound the loop */

    int registered = 0;
    uint32_t per_sec = 512 / ent_sz;
    uint8_t sec[512];
    for (uint32_t i = 0; i < n_ent; i++) {
        if ((i % per_sec) == 0) {
            if (embk_block_read(disk, ent_lba + i / per_sec, 1, sec) != EMBK_OK) break;
        }
        const uint8_t *e = sec + (i % per_sec) * ent_sz;

        /* An all-zero type GUID means "unused slot" -- GPT tables are sparse, so
         * this is the normal case, not an error. */
        int used = 0;
        for (int b = 0; b < 16; b++) if (e[b]) { used = 1; break; }
        if (!used) continue;

        uint64_t first = rd_le64(e + 32);
        uint64_t last  = rd_le64(e + 40);      /* INCLUSIVE -- hence the +1 below */
        if (last < first) continue;
        /* type is a 16-byte GUID; we log its first word purely as a hint. GPT
         * has no 1-byte type, so register_one_partition's `type` is cosmetic. */
        if (register_one_partition(disk, i + 1, (uint8_t)e[0], first, last - first + 1) == EMBK_OK)
            registered++;
    }
    kprintf("part: %s: GPT, %d partition(s)\n", disk->name, registered);
    return registered;
}

/* --- extended / logical partitions (the EBR chain) ------------------------
 * 🪤 THE TRAP: the two entries in an EBR use DIFFERENT BASES.
 *   entry 0 (the logical partition) is relative to THIS EBR's own LBA;
 *   entry 1 (the link to the next EBR) is relative to the EXTENDED partition's
 *           start -- NOT to this EBR.
 * Using one base for both is the classic bug: the chain appears to work for the
 * first logical partition and then walks off into nonsense.
 *
 * The loop is bounded and checks for revisits: a corrupt or hostile EBR can
 * point at itself, and an unbounded walk would hang the boot. */
static int ebr_walk(struct embk_block_device *disk, uint32_t ext_start, uint32_t *next_index) {
    uint32_t cur = ext_start;
    int registered = 0;

    for (int guard = 0; guard < 32; guard++) {
        uint8_t sec[512];
        if (embk_block_read(disk, cur, 1, sec) != EMBK_OK) break;
        if (sec[MBR_SIG_OFFSET] != MBR_SIG_0 || sec[MBR_SIG_OFFSET + 1] != MBR_SIG_1) break;

        const uint8_t *e0 = sec + MBR_PART_TABLE_OFFSET;
        const uint8_t *e1 = e0 + MBR_PART_ENTRY_SIZE;

        uint32_t l_start = rd_le32(e0 + 8);      /* relative to `cur` */
        uint32_t l_secs  = rd_le32(e0 + 12);
        if (e0[4] != MBR_TYPE_EMPTY && l_secs) {
            if (register_one_partition(disk, (*next_index), e0[4],
                                       (uint64_t)cur + l_start, l_secs) == EMBK_OK) {
                (*next_index)++;
                registered++;
            }
        }

        uint32_t n_start = rd_le32(e1 + 8);      /* relative to ext_start (see above) */
        if (e1[4] == MBR_TYPE_EMPTY || n_start == 0) break;
        uint32_t next = ext_start + n_start;
        if (next == cur) break;                  /* self-link: stop, don't spin */
        cur = next;
    }
    return registered;
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
        if (e0[4] == MBR_TYPE_GPT_PROTECTIVE)
            return gpt_scan(disk);      /* the MBR is a decoy; the truth is at LBA 1 */
    }

    int registered = 0;
    uint32_t logical_index = 5;      /* logicals are sda5+ regardless of how many primaries exist */
    for (uint32_t i = 0; i < MBR_PART_COUNT; i++) {
        const uint8_t *e = sector + MBR_PART_TABLE_OFFSET + i * MBR_PART_ENTRY_SIZE;
        uint8_t  type      = e[4];
        uint32_t start_lba = rd_le32(e + 8);
        uint32_t sectors   = rd_le32(e + 12);

        if (type == MBR_TYPE_EMPTY) {
            continue;
        }
        if (type == MBR_TYPE_EXTENDED || type == MBR_TYPE_EXTENDED_LBA) {
            /* The extended partition is a CONTAINER, not a volume: it gets no
             * block device of its own (nothing could mount it). Its logicals do,
             * numbered from 5 -- the universal convention, and the reason
             * make_part_name needed two digits. */
            registered += ebr_walk(disk, start_lba, &logical_index);
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
