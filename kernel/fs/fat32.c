#include "fat32.h"
#include "../include/ctype.h"
#include "../include/errno.h"
#include "../include/kprintf.h"
#include "../include/kstring.h"
#include "../mm/kheap.h"

#include <stdint.h>




// Convert a human filename ("HELLO.TXT") into the 11-byte on-disk FAT
// 8.3 form ("HELLO   TXT"): uppercase, space-padded, no dot.
// `out` must be at least 11 bytes. Does NOT null-terminate (it's a
// fixed 11-byte field, not a C string).
static void name_to_fat83(const char *name, char out[11]) {
    // 1. Fill all 11 bytes with spaces first.
    for (int i = 0; i < 11; i++) {
        out[i] = ' ';
    }

    // 2. Copy the name part (up to 8 chars) into out[0..7], uppercasing,
    //    until you hit a '.' or end-of-string.
    int dst = 0;
    while (*name && *name != '.' && dst < 8) {
        out[dst++] = (char)toupper((unsigned char)*name);
        name++;
    }

    // 3. If there was a '.', copy the extension (up to 3 chars) into
    //    out[8..10], uppercasing.
    if (*name == '.') {
        name++;
        dst = 8;
        while (*name && *name != '.' && dst < 11) {
            out[dst++] = (char)toupper((unsigned char)*name);
            name++;
        }
    }
}


int fat32_mount(struct embk_block_device *dev, struct fat32_volume *vol) {
    if (!dev || !vol) {
        return -EMBK_EINVAL;
    }

    // Read sector 0 (the boot sector / BPB) through the block layer.
    static uint8_t bootbuffer[512] __attribute__((aligned(4)));
    int rc = embk_block_read(dev, 0, 1, bootbuffer);
    if (rc != EMBK_OK) {
        kprintf("FAT32: failed to read boot sector: %s\n", embk_strerror(rc));
        return rc;
    }

    // Overlay the packed BPB struct onto the raw 512 bytes.
    const struct fat32_bpb *bpb = (const struct fat32_bpb *)bootbuffer;

    // Validate: must be 512-byte sectors, and the FAT32 sectors-per-fat
    // field must be non-zero (it's zero on FAT12/16, which we don't handle).
    if (bpb->bytes_per_sector != 512 || bpb->fat_size_32 == 0) {
        kprintf("FAT32: invalid BPB (bytes_per_sector=%u fat_size_32=%u)\n",
                (unsigned int)bpb->bytes_per_sector,
                (unsigned int)bpb->fat_size_32);
        return -EMBK_EINVAL;
    }

    // Also check the boot signature (0x55AA at offset 510) as a sanity check.
    if (bootbuffer[510] != 0x55 || bootbuffer[511] != 0xAA) {
        kprintf("FAT32: missing boot signature (got %x %x)\n",
                (unsigned int)bootbuffer[510], (unsigned int)bootbuffer[511]);
        return -EMBK_EINVAL;
    }

    // --- Copy fields from the BPB into our in-memory volume state ---
    vol->dev                 = dev;
    vol->bytes_per_sector    = bpb->bytes_per_sector;
    vol->sectors_per_cluster = bpb->sectors_per_cluster;
    vol->root_cluster        = bpb->root_cluster;
    vol->total_sectors       = bpb->total_sectors_32;
    vol->sectors_per_fat     = bpb->fat_size_32;
    vol->num_fats            = bpb->num_fats;

    // --- Compute region locations (absolute sector numbers) ---
    // FAT region starts right after the reserved sectors.
    vol->fat_start_sector  = bpb->reserved_sectors;
    // Data region starts after all the FATs.
    vol->data_start_sector = bpb->reserved_sectors
                           + (uint32_t)vol->num_fats * vol->sectors_per_fat;

    vol->mounted = true;

    // --- Validation prints (compare against minfo / mkfs output) ---
    kprintf("FAT32: bytes/sector=%u sectors/cluster=%u reserved=%u fats=%u\n",
            (unsigned int)vol->bytes_per_sector,
            (unsigned int)vol->sectors_per_cluster,
            (unsigned int)bpb->reserved_sectors,
            (unsigned int)vol->num_fats);

    kprintf("FAT32: sectors/fat=%u total=%u root_cluster=%u\n",
            (unsigned int)vol->sectors_per_fat,
            (unsigned int)vol->total_sectors,
            (unsigned int)vol->root_cluster);

    kprintf("FAT32: computed fat_start=%u data_start=%u\n",
            (unsigned int)vol->fat_start_sector,
            (unsigned int)vol->data_start_sector);

    kprintf("FAT32: mounted on %s\n", dev->name);

    return EMBK_OK;
}


// Convert a cluster number to its absolute sector number on the disk.
// Clusters are numbered starting at 2, and cluster 2 is the first thing
// in the data region.
static uint32_t cluster_to_sector(struct fat32_volume *vol, uint32_t cluster) {
    return vol->data_start_sector + (cluster - 2) * vol->sectors_per_cluster;
}

// Read the FAT entry for `cluster` - i.e. The next cluster in the chain.
// Returns the next cluster number (low 28 bits), or a value >= FAT32_EOC_MIN
// if this is the end of the chain. Returns 0 on read error (caller should
// treat 0 / errors carefully - 0 is also "free", but you won't be walking a free cluster chain).
static uint32_t fat_get_next_cluster(struct fat32_volume *vol, uint32_t cluster) {
    // 1. byte offset of this entry within the entire FAT region
    uint32_t fat_offset = cluster * 4; // 4 bytes per entry

    // 2. wich sector of the FAT, and byte offset within the FAT region
    uint32_t fat_sector = vol->fat_start_sector + (fat_offset / vol->bytes_per_sector);
    uint32_t offset_in_sector = fat_offset % vol->bytes_per_sector;

    // 3. read the entry from the disk
    static uint8_t fatbuf[4] __attribute__((aligned(4)));

    int rc = embk_block_read(vol->dev, fat_sector, 1, fatbuf);
    if (rc != EMBK_OK) {
        kprintf("FAT32: failed to read FAT entry %u: %s\n", cluster, embk_strerror(rc));
        return 0;
    }

    // 4. extract the 4 byte little-endian value at offset_in_sector
    uint32_t entry = (fatbuf[0]) | (fatbuf[1] << 8) | (fatbuf[2] << 16) | ((uint32_t)fatbuf[3] << 24);

    // 5. mask out the high 4 bits (0x0F) to get the actual cluster number
    uint32_t next_cluster = entry & FAT32_ENTRY_MASK;

    if (next_cluster >= FAT32_EOC_MIN) {
        return next_cluster;
    }

    return next_cluster;
}

// List the entries in the root directory.
void fat32_list_root(struct fat32_volume *vol) {
    if (!vol || !vol->mounted || !vol->dev) {
        kprintf("FAT32: volume not mounted\n");
        return;
    }

    kprintf("\n=== FAT32 Root Directory ===\n");

    uint32_t sectors_per_cluster = vol->sectors_per_cluster;
    uint32_t cluster_size        = vol->bytes_per_sector * sectors_per_cluster;
    uint32_t entries_per_cluster = cluster_size / sizeof(struct fat_dir_entry);

    uint8_t *dir_buffer = kmalloc(cluster_size);
    if (!dir_buffer) {
        kprintf("FAT32: failed to allocate %u bytes for directory cluster\n",
                (unsigned int)cluster_size);
        return;
    }

    uint32_t cluster = vol->root_cluster;
    bool done = false;

    while (!done && cluster >= 2 && cluster < FAT32_EOC_MIN && cluster != FAT32_FREE) {
        uint32_t first_sector = cluster_to_sector(vol, cluster);

        int rc = embk_block_read(vol->dev, first_sector, sectors_per_cluster, dir_buffer);
        if (rc != EMBK_OK) {
            kprintf("FAT32: failed to read directory cluster %u (sector %u): %s\n",
                    (unsigned int)cluster, (unsigned int)first_sector, embk_strerror(rc));
            break;
        }


        struct fat_dir_entry *entries = (struct fat_dir_entry *)dir_buffer;
        
        for (uint32_t i = 0; i < entries_per_cluster; i++) {
            struct fat_dir_entry *entry = &entries[i];

            // 0x00: no more entries anywhere in this directory.
            if (entry->name[0] == 0x00) {
                done = true;
                break;
            }
            // 0xE5: deleted entry, skip.
            if (entry->name[0] == 0xE5) {
                continue;
            }
            // Long-filename entry (all four low attr bits set): skip.
            if ((entry->attr & FAT32_ATTR_LFN) == FAT32_ATTR_LFN) {
                continue;
            }
            // Volume-label entry: skip.
            if (entry->attr & FAT32_ATTR_VOLUME_ID) {
                continue;
            }

            // --- Build a printable 8.3 name (strip padding, insert dot) ---
            char name[13];
            int name_len = 0;

            for (int j = 0; j < 8 && entry->name[j] != ' '; j++) {
                name[name_len++] = entry->name[j];
            }
            if (entry->name[8] != ' ') {
                name[name_len++] = '.';
                for (int j = 8; j < 11 && entry->name[j] != ' '; j++) {
                    name[name_len++] = entry->name[j];
                }
            }
            name[name_len] = '\0';

            uint32_t first_cluster =
                ((uint32_t)entry->first_cluster_high << 16) | entry->first_cluster_low;
            uint32_t file_size = entry->file_size;
            bool is_dir = (entry->attr & FAT32_ATTR_DIRECTORY) != 0;

            kprintf("%s %s %u bytes cluster=%u\n",
                    is_dir ? "<DIR> " : "<FILE>",
                    name,
                    (unsigned int)file_size,
                    (unsigned int)first_cluster);
        }

        if (done) {
            break;
        }

        // Advance to the next cluster in the directory's chain.
        cluster = fat_get_next_cluster(vol, cluster);
        if (cluster == 0) {
            kprintf("FAT32: broken cluster chain\n");
            break;
        }
    }

    kfree(dir_buffer);   // single cleanup point — every exit path reaches it
}


static int fat32_find_in_dir(struct fat32_volume *vol, uint32_t dir_cluster,
                             const char *name, struct fat_dir_entry *out) {
    // Convert target name to on-disk 8.3 form.
    char target[11];
    name_to_fat83(name, target);

    uint32_t sectors_per_cluster = vol->sectors_per_cluster;
    uint32_t cluster_size        = vol->bytes_per_sector * sectors_per_cluster;
    uint32_t entries_per_cluster = cluster_size / sizeof(struct fat_dir_entry);

    static uint8_t dir_buffer[4096] __attribute__((aligned(4)));
    if (cluster_size > sizeof(dir_buffer)) return -EMBK_EINVAL;

    uint32_t cluster = dir_cluster;
    while (cluster >= 2 && cluster < FAT32_EOC_MIN && cluster != FAT32_FREE) {
        uint32_t first_sector = cluster_to_sector(vol, cluster);
        int rc = embk_block_read(vol->dev, first_sector, sectors_per_cluster, dir_buffer);
        if (rc != EMBK_OK) return rc;

        struct fat_dir_entry *entries = (struct fat_dir_entry *)dir_buffer;
        for (uint32_t i = 0; i < entries_per_cluster; i++) {
            struct fat_dir_entry *entry = &entries[i];

            if (entry->name[0] == 0x00) return -EMBK_ENOENT;   // end of dir, not found
            if (entry->name[0] == 0xE5) continue;              // deleted
            if ((entry->attr & FAT32_ATTR_LFN) == FAT32_ATTR_LFN) continue;
            if (entry->attr & FAT32_ATTR_VOLUME_ID) continue;

            if (memcmp(entry->name, target, 11) == 0) {
                *out = *entry;
                return EMBK_OK;
            }
        }

        cluster = fat_get_next_cluster(vol, cluster);
        if (cluster == 0) return -EMBK_EIO;   // broken chain
    }
    return -EMBK_ENOENT;
}


static int fat32_read_file_data(struct fat32_volume *vol,
                                struct fat_dir_entry *entry,
                                uint8_t *buffer, uint32_t max_size) {
    uint32_t cluster = ((uint32_t)entry->first_cluster_high << 16)
                     | entry->first_cluster_low;
    uint32_t file_size = entry->file_size;

    // Read at most the file's size, and at most the buffer's capacity.
    uint32_t to_read = (file_size < max_size) ? file_size : max_size;

    uint32_t sectors_per_cluster = vol->sectors_per_cluster;
    uint32_t cluster_size = vol->bytes_per_sector * sectors_per_cluster;
    uint32_t bytes_read = 0;

    static uint8_t clusbuf[4096] __attribute__((aligned(4)));
    if (cluster_size > sizeof(clusbuf)) return -EMBK_EINVAL;

    while (cluster >= 2 && cluster < FAT32_EOC_MIN && cluster != FAT32_FREE
           && bytes_read < to_read) {
        uint32_t first_sector = cluster_to_sector(vol, cluster);
        int rc = embk_block_read(vol->dev, first_sector,
                                 sectors_per_cluster, clusbuf);
        if (rc != EMBK_OK) {
            return rc;
        }

        uint32_t remaining = to_read - bytes_read;
        uint32_t copy_len = (remaining < cluster_size) ? remaining : cluster_size;
        memcpy(buffer + bytes_read, clusbuf, copy_len);
        bytes_read += copy_len;

        if (bytes_read >= to_read) {
            break;
        }

        cluster = fat_get_next_cluster(vol, cluster);
        if (cluster == 0) {
            return -EMBK_EIO;
        }
    }

    return (int)bytes_read;
}


int fat32_read(struct fat32_volume *vol, const char *name,
               uint8_t *buffer, uint32_t max_size) {
    if (!vol || !vol->mounted || !name || !buffer) return -EMBK_EINVAL;

    struct fat_dir_entry entry;
    int rc = fat32_find_in_dir(vol, vol->root_cluster, name, &entry);
    if (rc != EMBK_OK) return rc;

    return fat32_read_file_data(vol, &entry, buffer, max_size);
}