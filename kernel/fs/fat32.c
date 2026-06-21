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

// Get the next cluster in a chain. Wraps fat32_read_fat_entry (which
// correctly handles the byte-offset within the FAT sector). Returns the
// next cluster, or a value >= FAT32_EOC_MIN for end-of-chain, or 0 on error.
static uint32_t fat_get_next_cluster(struct fat32_volume *vol, uint32_t cluster) {
    uint32_t next = 0;
    int rc = fat32_read_fat_entry(vol, cluster, &next);
    if (rc != EMBK_OK) {
        return 0;   // error — caller treats 0 as broken chain
    }
    return next;
}

static uint32_t fat32_cluster_count(struct fat32_volume *vol) {
    uint32_t data_sectors = vol->total_sectors - vol->data_start_sector;
    return data_sectors / vol->sectors_per_cluster;
}

static bool fat32_valid_cluster(struct fat32_volume *vol, uint32_t cluster) {
    if (!vol) {
        return false;
    }
    if (cluster < 2) {
        return false;
    }
    uint32_t max_cluster = fat32_cluster_count(vol) + 1;
    return cluster <= max_cluster;
}

static int fat32_read_fat_entry(struct fat32_volume *vol, uint32_t cluster,
                                uint32_t *out_value) {
    if (!fat32_valid_cluster(vol, cluster)) {
        return -EMBK_EINVAL;
    }

    uint32_t fat_offset = cluster * 4;
    uint32_t sector_index = fat_offset / vol->bytes_per_sector;
    uint32_t offset_in_sector = fat_offset % vol->bytes_per_sector;
    uint32_t fat_sector = vol->fat_start_sector + sector_index;
    uint32_t sectors = (offset_in_sector <= vol->bytes_per_sector - 4) ? 1 : 2;

    uint8_t fatbuf[1024] __attribute__((aligned(4)));
    int rc = embk_block_read(vol->dev, fat_sector, sectors, fatbuf);
    if (rc != EMBK_OK) {
        return rc;
    }

    uint32_t value = fatbuf[offset_in_sector] |
                     (fatbuf[offset_in_sector + 1] << 8) |
                     (fatbuf[offset_in_sector + 2] << 16) |
                     ((uint32_t)fatbuf[offset_in_sector + 3] << 24);
    if (out_value) {
        *out_value = value & FAT32_ENTRY_MASK;
    }
    return EMBK_OK;
}

static int fat32_write_fat_entry(struct fat32_volume *vol, uint32_t cluster,
                                 uint32_t value) {
    if (!fat32_valid_cluster(vol, cluster)) {
        return -EMBK_EINVAL;
    }

    uint32_t fat_offset = cluster * 4;
    uint32_t sector_index = fat_offset / vol->bytes_per_sector;
    uint32_t offset_in_sector = fat_offset % vol->bytes_per_sector;
    uint32_t sectors = (offset_in_sector <= vol->bytes_per_sector - 4) ? 1 : 2;
    uint32_t masked_value = value & FAT32_ENTRY_MASK;

    uint8_t fatbuf[1024] __attribute__((aligned(4)));

    for (uint8_t fat = 0; fat < vol->num_fats; fat++) {
        uint32_t fat_sector = vol->fat_start_sector + fat * vol->sectors_per_fat + sector_index;
        int rc = embk_block_read(vol->dev, fat_sector, sectors, fatbuf);
        if (rc != EMBK_OK) {
            return rc;
        }

        fatbuf[offset_in_sector] = (uint8_t)(masked_value & 0xFF);
        fatbuf[offset_in_sector + 1] = (uint8_t)((masked_value >> 8) & 0xFF);
        fatbuf[offset_in_sector + 2] = (uint8_t)((masked_value >> 16) & 0xFF);
        fatbuf[offset_in_sector + 3] = (uint8_t)((masked_value >> 24) & 0xFF);

        rc = embk_block_write(vol->dev, fat_sector, sectors, fatbuf);
        if (rc != EMBK_OK) {
            return rc;
        }
    }

    return EMBK_OK;
}

// Update the FSInfo sector (sector 1) free-cluster count and optional
// next-free hint. `delta_free` is added to the current free count
// (positive to increase, negative to decrease). If `set_next` is true,
// `next_free_hint` is written to the FSInfo next-free field.
static int fat32_update_fsinfo_delta(struct fat32_volume *vol,
                                     int32_t delta_free,
                                     uint32_t next_free_hint,
                                     bool set_next) {
    if (!vol || !vol->dev) return -EMBK_EINVAL;

    uint32_t sector = 1; // FSInfo is at sector 1
    uint8_t *buf = kmalloc(vol->bytes_per_sector);
    if (!buf) return -EMBK_ENOMEM;

    int rc = embk_block_read(vol->dev, sector, 1, buf);
    if (rc != EMBK_OK) {
        kfree(buf);
        return rc;
    }

    // Read current free count (little-endian at offset 0x1E8)
    uint32_t free_count = (uint32_t)buf[0x1E8] |
                          ((uint32_t)buf[0x1E9] << 8) |
                          ((uint32_t)buf[0x1EA] << 16) |
                          ((uint32_t)buf[0x1EB] << 24);

    if (delta_free != 0) {
        int64_t new_count = (int64_t)free_count + (int64_t)delta_free;
        if (new_count < 0) new_count = 0;
        free_count = (uint32_t)new_count;

        buf[0x1E8] = (uint8_t)(free_count & 0xFF);
        buf[0x1E9] = (uint8_t)((free_count >> 8) & 0xFF);
        buf[0x1EA] = (uint8_t)((free_count >> 16) & 0xFF);
        buf[0x1EB] = (uint8_t)((free_count >> 24) & 0xFF);
    }

    if (set_next) {
        uint32_t hint = next_free_hint;
        buf[0x1EC] = (uint8_t)(hint & 0xFF);
        buf[0x1ED] = (uint8_t)((hint >> 8) & 0xFF);
        buf[0x1EE] = (uint8_t)((hint >> 16) & 0xFF);
        buf[0x1EF] = (uint8_t)((hint >> 24) & 0xFF);
    }

    rc = embk_block_write(vol->dev, sector, 1, buf);
    if (rc != EMBK_OK) {
        kfree(buf);
        return rc;
    }

    kfree(buf);
    return EMBK_OK;
}

static int fat32_find_free_cluster(struct fat32_volume *vol, uint32_t start,
                                   uint32_t *out_cluster) {
    uint32_t cluster_count = fat32_cluster_count(vol);
    uint32_t max_cluster = cluster_count + 1;
    if (start < 2) {
        start = 2;
    }
    if (start > max_cluster) {
        start = 2;
    }

    for (uint32_t cluster = start; cluster <= max_cluster; cluster++) {
        uint32_t value;
        int rc = fat32_read_fat_entry(vol, cluster, &value);
        if (rc != EMBK_OK) {
            return rc;
        }
        if (value == FAT32_FREE) {
            *out_cluster = cluster;
            return EMBK_OK;
        }
    }

    for (uint32_t cluster = 2; cluster < start; cluster++) {
        uint32_t value;
        int rc = fat32_read_fat_entry(vol, cluster, &value);
        if (rc != EMBK_OK) {
            return rc;
        }
        if (value == FAT32_FREE) {
            *out_cluster = cluster;
            return EMBK_OK;
        }
    }

    return -EMBK_ENOSPC;
}

static int fat32_alloc_cluster_chain(struct fat32_volume *vol, uint32_t count,
                                     uint32_t *out_head, uint32_t *out_tail) {
    if (count == 0) {
        return -EMBK_EINVAL;
    }

    uint32_t first = 0;
    uint32_t prev = 0;
    uint32_t search_start = 2;
    uint32_t cluster_count = fat32_cluster_count(vol);
    uint32_t max_cluster = cluster_count + 1;

    for (uint32_t i = 0; i < count; i++) {
        uint32_t free_cluster;
        int rc = fat32_find_free_cluster(vol, search_start, &free_cluster);
        if (rc != EMBK_OK) {
            if (first) {
                fat32_free_cluster_chain(vol, first);
            }
            return rc;
        }

        if (!first) {
            first = free_cluster;
        }

        if (prev) {
            rc = fat32_write_fat_entry(vol, prev, free_cluster);
            if (rc != EMBK_OK) {
                fat32_free_cluster_chain(vol, first);
                return rc;
            }
        }

        prev = free_cluster;
        search_start = free_cluster + 1;
    }

    int rc = fat32_write_fat_entry(vol, prev, FAT32_EOC_MIN);
    if (rc != EMBK_OK) {
        fat32_free_cluster_chain(vol, first);
        return rc;
    }

    // Update FSInfo: decrease free count by number of allocated clusters,
    // and set the next-free hint to the cluster just after the last
    // search start (a reasonable hint). Ignore FSInfo errors.
    uint32_t next_hint = (search_start <= max_cluster) ? search_start : 2;
    int rc2 = fat32_update_fsinfo_delta(vol, -((int32_t)count), next_hint, true);
    if (rc2 != EMBK_OK) {
        kprintf("FAT32: warning - failed to update FSInfo after alloc: %s\n", embk_strerror(rc2));
    }

    if (out_head) {
        *out_head = first;
    }
    if (out_tail) {
        *out_tail = prev;
    }
    return EMBK_OK;
}

static int fat32_free_cluster_chain(struct fat32_volume *vol, uint32_t cluster) {
    if (!(cluster >= 2 && cluster < FAT32_EOC_MIN && cluster != FAT32_FREE)) {
        return EMBK_OK;
    }

    uint32_t freed_head = cluster;
    uint32_t freed_count = 0;

    while (cluster >= 2 && cluster < FAT32_EOC_MIN && cluster != FAT32_FREE) {
        uint32_t next;
        int rc = fat32_read_fat_entry(vol, cluster, &next);
        if (rc != EMBK_OK) {
            return rc;
        }

        rc = fat32_write_fat_entry(vol, cluster, FAT32_FREE);
        if (rc != EMBK_OK) {
            return rc;
        }

        freed_count++;

        if (next >= FAT32_EOC_MIN || next == FAT32_FREE) {
            break;
        }

        cluster = next;
    }

    // Update FSInfo: increase free count and set next-free hint to the
    // first freed cluster. Ignore FSInfo errors.
    int rc2 = fat32_update_fsinfo_delta(vol, (int32_t)freed_count, freed_head, true);
    if (rc2 != EMBK_OK) {
        kprintf("FAT32: warning - failed to update FSInfo after free: %s\n", embk_strerror(rc2));
    }

    return EMBK_OK;
}

static int fat32_find_dir_entry_location(struct fat32_volume *vol,
                                        uint32_t dir_cluster,
                                        const char *name,
                                        uint32_t *out_cluster,
                                        uint32_t *out_index,
                                        struct fat_dir_entry *out_entry) {
    char target[11];
    name_to_fat83(name, target);

    uint32_t sectors_per_cluster = vol->sectors_per_cluster;
    uint32_t cluster_size = vol->bytes_per_sector * sectors_per_cluster;
    uint32_t entries_per_cluster = cluster_size / sizeof(struct fat_dir_entry);

    uint8_t *buffer = kmalloc(cluster_size);
    if (!buffer) {
        return -EMBK_ENOMEM;
    }

    uint32_t cluster = dir_cluster;
    while (cluster >= 2 && cluster < FAT32_EOC_MIN && cluster != FAT32_FREE) {
        uint32_t first_sector = cluster_to_sector(vol, cluster);
        int rc = embk_block_read(vol->dev, first_sector,
                                 sectors_per_cluster, buffer);
        if (rc != EMBK_OK) {
            kfree(buffer);
            return rc;
        }

        struct fat_dir_entry *entries = (struct fat_dir_entry *)buffer;
        for (uint32_t i = 0; i < entries_per_cluster; i++) {
            struct fat_dir_entry *entry = &entries[i];
            if (entry->name[0] == 0x00) {
                kfree(buffer);
                return -EMBK_ENOENT;
            }
            if (entry->name[0] == 0xE5) {
                continue;
            }
            if ((entry->attr & FAT32_ATTR_LFN) == FAT32_ATTR_LFN) {
                continue;
            }
            if (entry->attr & FAT32_ATTR_VOLUME_ID) {
                continue;
            }

            if (memcmp(entry->name, target, 11) == 0) {
                if (out_cluster) *out_cluster = cluster;
                if (out_index) *out_index = i;
                if (out_entry) *out_entry = *entry;
                kfree(buffer);
                return EMBK_OK;
            }
        }

        cluster = fat_get_next_cluster(vol, cluster);
        if (cluster == 0) {
            kfree(buffer);
            return -EMBK_EIO;
        }
    }

    kfree(buffer);
    return -EMBK_ENOENT;
}

static int fat32_find_free_dir_slot(struct fat32_volume *vol,
                                    uint32_t dir_cluster,
                                    uint32_t *out_cluster,
                                    uint32_t *out_index,
                                    uint8_t *cluster_buffer) {
    uint32_t sectors_per_cluster = vol->sectors_per_cluster;
    uint32_t cluster_size = vol->bytes_per_sector * sectors_per_cluster;

    uint32_t cluster = dir_cluster;
    uint32_t last_cluster = 0;
    while (cluster >= 2 && cluster < FAT32_EOC_MIN && cluster != FAT32_FREE) {
        last_cluster = cluster;
        uint32_t first_sector = cluster_to_sector(vol, cluster);
        int rc = embk_block_read(vol->dev, first_sector,
                                 sectors_per_cluster, cluster_buffer);
        if (rc != EMBK_OK) {
            return rc;
        }

        struct fat_dir_entry *entries = (struct fat_dir_entry *)cluster_buffer;
        uint32_t entries_per_cluster = cluster_size / sizeof(struct fat_dir_entry);
        for (uint32_t i = 0; i < entries_per_cluster; i++) {
            struct fat_dir_entry *entry = &entries[i];
            if (entry->name[0] == 0x00 || entry->name[0] == 0xE5) {
                *out_cluster = cluster;
                *out_index = i;
                return EMBK_OK;
            }
        }

        uint32_t next = fat_get_next_cluster(vol, cluster);
        if (next == 0) {
            return -EMBK_EIO;
        }
        cluster = next;
    }

    // No free slot found; allocate one more cluster for the directory.
    uint32_t new_cluster;
    int rc = fat32_alloc_cluster_chain(vol, 1, &new_cluster, NULL);
    if (rc != EMBK_OK) {
        return rc;
    }

    rc = fat32_write_fat_entry(vol, last_cluster, new_cluster);
    if (rc != EMBK_OK) {
        fat32_free_cluster_chain(vol, new_cluster);
        return rc;
    }

    memset(cluster_buffer, 0, cluster_size);
    uint32_t first_sector = cluster_to_sector(vol, new_cluster);
    rc = embk_block_write(vol->dev, first_sector,
                          sectors_per_cluster, cluster_buffer);
    if (rc != EMBK_OK) {
        fat32_free_cluster_chain(vol, new_cluster);
        return rc;
    }

    *out_cluster = new_cluster;
    *out_index = 0;
    return EMBK_OK;
}

static int fat32_find_parent_dir(struct fat32_volume *vol,
                                 const char *path,
                                 uint32_t *out_parent_cluster,
                                 char *out_name) {
    if (!vol || !path || !*path || !out_name) {
        return -EMBK_EINVAL;
    }

    uint32_t cluster = vol->root_cluster;
    const char *p = path;
    if (*p == '/') {
        p++;
    }

    const char *segment = p;
    while (true) {
        while (*p == '/') {
            p++;
        }
        if (*p == '\0') {
            return -EMBK_EINVAL;
        }

        segment = p;
        while (*p != '/' && *p != '\0') {
            p++;
        }

        size_t len = p - segment;
        if (len == 0) {
            return -EMBK_EINVAL;
        }
        if (len >= 12) {
            return -EMBK_EINVAL;
        }

        if (*p == '\0') {
            memcpy(out_name, segment, len);
            out_name[len] = '\0';
            *out_parent_cluster = cluster;
            return EMBK_OK;
        }

        char component[256];
        if (len >= sizeof(component)) {
            return -EMBK_EINVAL;
        }
        memcpy(component, segment, len);
        component[len] = '\0';

        struct fat_dir_entry entry;
        int rc = fat32_find_in_dir(vol, cluster, component, &entry);
        if (rc != EMBK_OK) {
            return rc;
        }
        if (!(entry.attr & FAT32_ATTR_DIRECTORY)) {
            return -EMBK_ENOTDIR;
        }

        cluster = ((uint32_t)entry.first_cluster_high << 16) |
                  entry.first_cluster_low;
        if (!fat32_valid_cluster(vol, cluster)) {
            return -EMBK_EIO;
        }

        if (*p == '/') {
            p++;
        }
    }
}

static int fat32_find_path(struct fat32_volume *vol, const char *path,
                           struct fat_dir_entry *out_entry) {
    if (!vol || !path || !*path) {
        return -EMBK_EINVAL;
    }

    uint32_t cluster = vol->root_cluster;
    const char *p = path;
    if (*p == '/') {
        p++;
    }

    while (true) {
        while (*p == '/') {
            p++;
        }
        if (*p == '\0') {
            return -EMBK_EINVAL;
        }

        const char *segment = p;
        while (*p != '/' && *p != '\0') {
            p++;
        }

        size_t len = p - segment;
        if (len == 0) {
            return -EMBK_EINVAL;
        }
        if (len >= 256) {
            return -EMBK_EINVAL;
        }

        char component[256];
        memcpy(component, segment, len);
        component[len] = '\0';

        struct fat_dir_entry entry;
        int rc = fat32_find_in_dir(vol, cluster, component, &entry);
        if (rc != EMBK_OK) {
            return rc;
        }

        if (*p == '\0') {
            if (out_entry) {
                *out_entry = entry;
            }
            return EMBK_OK;
        }

        if (!(entry.attr & FAT32_ATTR_DIRECTORY)) {
            return -EMBK_ENOTDIR;
        }

        cluster = ((uint32_t)entry.first_cluster_high << 16) |
                  entry.first_cluster_low;
        if (!fat32_valid_cluster(vol, cluster)) {
            return -EMBK_EIO;
        }

        if (*p == '/') {
            p++;
        }
    }
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


static int fat32_write_data_to_chain(struct fat32_volume *vol,
                                     uint32_t start_cluster,
                                     const uint8_t *buffer, uint32_t size) {
    if (!vol || !buffer) {
        return -EMBK_EINVAL;
    }
    if (size == 0) {
        return EMBK_OK;
    }

    uint32_t sectors_per_cluster = vol->sectors_per_cluster;
    uint32_t cluster_size = vol->bytes_per_sector * sectors_per_cluster;
    uint32_t bytes_written = 0;
    uint32_t cluster = start_cluster;

    uint8_t *clusbuf = kmalloc(cluster_size);
    if (!clusbuf) {
        return -EMBK_ENOMEM;
    }

    while (cluster >= 2 && cluster < FAT32_EOC_MIN && cluster != FAT32_FREE
           && bytes_written < size) {
        uint32_t first_sector = cluster_to_sector(vol, cluster);
        uint32_t remaining = size - bytes_written;
        uint32_t write_len = (remaining < cluster_size) ? remaining : cluster_size;

        memcpy(clusbuf, buffer + bytes_written, write_len);
        if (write_len < cluster_size) {
            memset(clusbuf + write_len, 0, cluster_size - write_len);
        }

        int rc = embk_block_write(vol->dev, first_sector,
                                  sectors_per_cluster, clusbuf);
        if (rc != EMBK_OK) {
            kfree(clusbuf);
            return rc;
        }

        bytes_written += write_len;
        if (bytes_written >= size) {
            break;
        }

        cluster = fat_get_next_cluster(vol, cluster);
        if (cluster == 0) {
            kfree(clusbuf);
            return -EMBK_EIO;
        }
    }

    kfree(clusbuf);
    return (int)bytes_written;
}

int fat32_read(struct fat32_volume *vol, const char *name,
               uint8_t *buffer, uint32_t max_size) {
    if (!vol || !vol->mounted || !name || !buffer) return -EMBK_EINVAL;

    struct fat_dir_entry entry;
    int rc = fat32_find_path(vol, name, &entry);
    if (rc != EMBK_OK) return rc;

    if (entry.attr & FAT32_ATTR_DIRECTORY) {
        return -EMBK_EISDIR;
    }

    return fat32_read_file_data(vol, &entry, buffer, max_size);
}

int fat32_write(struct fat32_volume *vol, const char *path,
                const uint8_t *buffer, uint32_t size) {
    if (!vol || !vol->mounted || !path || !buffer) {
        return -EMBK_EINVAL;
    }

    uint32_t parent_cluster;
    char filename[256];
    int rc = fat32_find_parent_dir(vol, path, &parent_cluster, filename);
    if (rc != EMBK_OK) {
        return rc;
    }

    uint32_t entry_cluster = 0;
    uint32_t entry_index = 0;
    struct fat_dir_entry existing_entry;
    bool exists = true;
    rc = fat32_find_dir_entry_location(vol, parent_cluster, filename,
                                       &entry_cluster, &entry_index,
                                       &existing_entry);
    if (rc == -EMBK_ENOENT) {
        exists = false;
    } else if (rc != EMBK_OK) {
        return rc;
    }

    uint32_t old_head = 0;
    if (exists) {
        old_head = ((uint32_t)existing_entry.first_cluster_high << 16) |
                   existing_entry.first_cluster_low;
        if (existing_entry.attr & FAT32_ATTR_DIRECTORY) {
            return -EMBK_EISDIR;
        }
    }

    uint32_t cluster_size = vol->bytes_per_sector * vol->sectors_per_cluster;
    uint32_t clusters_needed = (size == 0) ? 0 :
        ((size + cluster_size - 1) / cluster_size);

    uint32_t new_head = 0;
    if (clusters_needed > 0) {
        rc = fat32_alloc_cluster_chain(vol, clusters_needed, &new_head, NULL);
        if (rc != EMBK_OK) {
            return rc;
        }
    }

    if (exists && old_head >= 2) {
        rc = fat32_free_cluster_chain(vol, old_head);
        if (rc != EMBK_OK) {
            if (new_head) {
                fat32_free_cluster_chain(vol, new_head);
            }
            return rc;
        }
    }

    if (size > 0 && new_head) {
        rc = fat32_write_data_to_chain(vol, new_head, buffer, size);
        if (rc < 0) {
            if (new_head) {
                fat32_free_cluster_chain(vol, new_head);
            }
            return rc;
        }
    }

    uint8_t *cluster_buffer = kmalloc(cluster_size);
    if (!cluster_buffer) {
        if (new_head) {
            fat32_free_cluster_chain(vol, new_head);
        }
        return -EMBK_ENOMEM;
    }

    uint32_t dir_cluster = parent_cluster;
    if (!exists) {
        rc = fat32_find_free_dir_slot(vol, parent_cluster,
                                      &dir_cluster, &entry_index,
                                      cluster_buffer);
        if (rc != EMBK_OK) {
            kfree(cluster_buffer);
            if (new_head) {
                fat32_free_cluster_chain(vol, new_head);
            }
            return rc;
        }
    }

    uint32_t first_sector = cluster_to_sector(vol, dir_cluster);
    rc = embk_block_read(vol->dev, first_sector,
                         vol->sectors_per_cluster, cluster_buffer);
    if (rc != EMBK_OK) {
        kfree(cluster_buffer);
        if (!exists && new_head) {
            fat32_free_cluster_chain(vol, new_head);
        }
        return rc;
    }

    struct fat_dir_entry *entries = (struct fat_dir_entry *)cluster_buffer;
    if (!exists) {
        memset(&entries[entry_index], 0, sizeof(*entries));
        name_to_fat83(filename, (char *)entries[entry_index].name);
        entries[entry_index].attr = FAT32_ATTR_ARCHIVE;
    }

    entries[entry_index].first_cluster_high = (uint16_t)(new_head >> 16);
    entries[entry_index].first_cluster_low = (uint16_t)(new_head & 0xFFFF);
    entries[entry_index].file_size = size;

    rc = embk_block_write(vol->dev, first_sector,
                          vol->sectors_per_cluster, cluster_buffer);
    kfree(cluster_buffer);
    if (rc != EMBK_OK) {
        if (new_head) {
            fat32_free_cluster_chain(vol, new_head);
        }
        return rc;
    }

    return (int)size;
}