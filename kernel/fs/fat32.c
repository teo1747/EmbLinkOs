#include "fat32.h"
#include "../include/ctype.h"
#include "../include/errno.h"
#include "../include/kprintf.h"
#include "../include/kstring.h"
#include "../mm/kheap.h"

#include <stdint.h>

// Minimum valid FAT date: 1980-01-01. Month and day must be >= 1; a literal
// zero packs to 1980-00-00, which stricter fsck builds reject.
#define FAT32_EPOCH_DATE  (((0) << 9) | ((1) << 5) | (1))




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

struct fat32_lfn_entry {
    uint8_t order;
    uint16_t name1[5];
    uint8_t attr;
    uint8_t type;
    uint8_t checksum;
    uint16_t name2[6];
    uint16_t first_cluster_low;
    uint16_t name3[2];
} __attribute__((packed));

static uint8_t fat32_short_name_checksum(const uint8_t name[11]) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) {
        sum = ((sum & 1) ? 0x80 : 0) + (sum >> 1) + name[i];
    }
    return sum;
}

static bool fat32_name_is_short(const char *name) {
    int length = 0;
    bool seen_dot = false;

    if (!name || !*name) {
        return false;
    }

    while (*name) {
        char c = *name;
        if (c == '.') {
            if (seen_dot) {
                return false;
            }
            seen_dot = true;
            length = 0;
            name++;
            continue;
        }

        if (c == ' ' || c == '+' || c == ',' || c == ';' || c == '=' || c == '[' ||
            c == ']' || c == '/' || c == '\\' || c == '"' || c < 0) {
            return false;
        }

        if (++length > (seen_dot ? 3 : 8)) {
            return false;
        }

        name++;
    }

    return true;
}

// FIX #1: the old version did `while (*p++) { len++; p++; }` which advances
// the pointer twice per iteration, reading past the end of the string into
// uninitialized stack and producing a garbage slot count. Walk by index.
static uint32_t fat32_lfn_slot_count(const char *name) {
    if (fat32_name_is_short(name)) {
        return 0;
    }
    uint32_t len = 0;
    while (name[len]) {
        len++;
    }
    // 13 UTF-16 code units per LFN slot, +1 for the NUL terminator, round up.
    return (len + 13) / 13;
}

// FIX #4: keep the extension. The old base-copy loop exited at base_len == 6
// with `p` stranded mid-name, so it never reached the '.' and the extension
// was silently dropped (LONG_N~1 with no .TXT). Find the last dot up front,
// stop the base copy at it, then copy the extension independently.
static void fat32_generate_short_name(const char *name, char out[11]) {
    for (int i = 0; i < 11; i++) {
        out[i] = ' ';
    }

    if (fat32_name_is_short(name)) {
        name_to_fat83(name, out);
        return;
    }

    const char *dot = NULL;
    for (const char *q = name; *q; q++) {
        if (*q == '.') {
            dot = q;                 // last dot wins
        }
    }

    const char *p = name;
    int base_len = 0;
    while (*p && p != dot && base_len < 6) {
        char c = *p++;
        out[base_len++] = (c == ' ') ? '_' : (char)toupper((unsigned char)c);
    }
    out[6] = '~';
    out[7] = '1';

    if (dot) {
        const char *e = dot + 1;
        int ext_pos = 8;
        while (*e && ext_pos < 11) {
            out[ext_pos++] = (char)toupper((unsigned char)*e);
            e++;
        }
    }
}

static uint32_t fat32_encode_lfn_name(const char *name, uint16_t *out, uint32_t max_chars) {
    uint32_t count = 0;
    while (*name && count + 1 < max_chars) {
        unsigned char c = (unsigned char)*name++;
        out[count++] = (uint16_t)(c < 0x80 ? c : '?');
    }
    if (count < max_chars) {
        out[count++] = 0;
    }
    return count;
}

static void fat32_fill_lfn_entry(struct fat32_lfn_entry *entry,
                                 uint8_t order,
                                 uint8_t checksum,
                                 const uint16_t *name_chars,
                                 uint32_t name_offset,
                                 uint32_t chars_remaining) {
    memset(entry, 0, sizeof(struct fat32_lfn_entry));
    entry->order = order;
    entry->attr = FAT32_ATTR_LFN;
    entry->type = 0;
    entry->checksum = checksum;
    entry->first_cluster_low = 0;

    for (int i = 0; i < 5; i++) {
        uint16_t ch = (chars_remaining > 0) ? name_chars[name_offset++] : 0xFFFF;
        entry->name1[i] = ch;
        if (chars_remaining > 0) chars_remaining--;
    }
    for (int i = 0; i < 6; i++) {
        uint16_t ch = (chars_remaining > 0) ? name_chars[name_offset++] : 0xFFFF;
        entry->name2[i] = ch;
        if (chars_remaining > 0) chars_remaining--;
    }
    for (int i = 0; i < 2; i++) {
        uint16_t ch = (chars_remaining > 0) ? name_chars[name_offset++] : 0xFFFF;
        entry->name3[i] = ch;
        if (chars_remaining > 0) chars_remaining--;
    }
}

static bool fat32_reconstruct_lfn_name(char *out, size_t out_size,
                                       const struct fat32_lfn_entry *entries,
                                       uint32_t count) {
    if (count == 0 || count > 20 || out_size == 0) {
        return false;
    }

    const struct fat32_lfn_entry *ordered[20] = {0};
    for (uint32_t i = 0; i < count; i++) {
        uint32_t seq = entries[i].order & 0x1F;
        if (seq == 0 || seq > count) {
            return false;
        }
        ordered[seq - 1] = &entries[i];
    }

    size_t pos = 0;
    for (uint32_t seq = 0; seq < count; seq++) {
        const struct fat32_lfn_entry *entry = ordered[seq];
        if (!entry) {
            return false;
        }

        uint16_t name1[5];
        uint16_t name2[6];
        uint16_t name3[2];
        memcpy(name1, entry->name1, sizeof(name1));
        memcpy(name2, entry->name2, sizeof(name2));
        memcpy(name3, entry->name3, sizeof(name3));

        const uint16_t *parts[3] = {name1, name2, name3};
        const int lengths[3] = {5, 6, 2};

        for (int part = 0; part < 3; part++) {
            for (int j = 0; j < lengths[part]; j++) {
                uint16_t ch = parts[part][j];
                if (ch == 0x0000) {
                    out[pos] = '\0';
                    return true;
                }
                if (ch == 0xFFFF) {
                    break;
                }
                if (pos + 1 >= out_size) {
                    return false;
                }
                out[pos++] = (char)(ch < 0x80 ? ch : '?');
            }
        }
    }

    out[pos] = '\0';
    return true;
}

static bool fat32_match_dir_entry_name(const char *name,
                                       const struct fat_dir_entry *entry,
                                       const struct fat32_lfn_entry *lfn_entries,
                                       uint32_t lfn_count) {
    if (lfn_count > 0) {
        char longname[260];
        if (fat32_reconstruct_lfn_name(longname, sizeof(longname), lfn_entries, lfn_count) &&
            strcmp(name, longname) == 0) {
            return true;
        }
    }

    char short_name[11];
    fat32_generate_short_name(name, short_name);
    return memcmp(entry->name, short_name, 11) == 0;
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
    uint32_t sectors_per_cluster = vol->sectors_per_cluster;
    uint32_t cluster_size = vol->bytes_per_sector * sectors_per_cluster;
    uint32_t entries_per_cluster = cluster_size / sizeof(struct fat_dir_entry);

    uint8_t *buffer = kmalloc(cluster_size);
    if (!buffer) {
        return -EMBK_ENOMEM;
    }

    struct fat32_lfn_entry lfn_entries[20];
    uint32_t lfn_count = 0;

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
                lfn_count = 0;
                continue;
            }
            if ((entry->attr & FAT32_ATTR_LFN) == FAT32_ATTR_LFN) {
                if (lfn_count < sizeof(lfn_entries) / sizeof(lfn_entries[0])) {
                    lfn_entries[lfn_count++] = *(struct fat32_lfn_entry *)entry;
                }
                continue;
            }
            if (entry->attr & FAT32_ATTR_VOLUME_ID) {
                lfn_count = 0;
                continue;
            }

            if (fat32_match_dir_entry_name(name, entry, lfn_entries, lfn_count)) {
                if (out_cluster) *out_cluster = cluster;
                if (out_index) *out_index = i;
                if (out_entry) *out_entry = *entry;
                kfree(buffer);
                return EMBK_OK;
            }
            lfn_count = 0;
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

// FIX #2: find a run of `needed_slots` contiguous free directory slots.
//
// The old version stopped at the first 0x00 end-of-directory marker treating
// it as a single free slot, so any entry needing >= 2 contiguous slots (i.e.
// anything with an LFN) failed to find room in the normal "free tail" case and
// fell through to allocating a whole new directory cluster — while leaving the
// old 0x00 marker in the first cluster, which then made directory scans stop
// before ever reaching the new cluster. Result: the file was written but never
// found again (ENOENT on read back).
//
// Correct behavior: 0x00 means this slot AND every slot after it in the cluster
// is free. If the trailing space is large enough, use it. If it isn't, convert
// that trailing run to 0xE5 deleted markers (so future scans don't stop here),
// then grow the directory by one cluster. Also reuse runs of 0xE5 left behind
// by deleted entries.
static int fat32_find_free_dir_slot(struct fat32_volume *vol,
                                    uint32_t dir_cluster,
                                    uint32_t needed_slots,
                                    uint32_t *out_cluster,
                                    uint32_t *out_index,
                                    uint8_t *cluster_buffer) {
    uint32_t sectors_per_cluster = vol->sectors_per_cluster;
    uint32_t cluster_size = vol->bytes_per_sector * sectors_per_cluster;
    uint32_t entries_per_cluster = cluster_size / sizeof(struct fat_dir_entry);

    if (needed_slots == 0) {
        needed_slots = 1;
    }
    if (needed_slots > entries_per_cluster) {
        // A full LFN+short set has to live inside a single cluster here.
        return -EMBK_EINVAL;
    }

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
        uint32_t run_start = 0;
        uint32_t run_len = 0;

        for (uint32_t i = 0; i < entries_per_cluster; i++) {
            uint8_t marker = entries[i].name[0];

            if (marker == 0x00) {
                // End-of-directory: this slot and everything after it in this
                // cluster is free.
                if (run_len == 0) {
                    run_start = i;
                }
                if (entries_per_cluster - run_start >= needed_slots) {
                    *out_cluster = cluster;
                    *out_index = run_start;
                    return EMBK_OK;
                }
                // Not enough room before the cluster ends. Convert the trailing
                // free run into deleted markers so later scans don't stop at
                // this 0x00, then fall through to grow the directory.
                for (uint32_t k = run_start; k < entries_per_cluster; k++) {
                    entries[k].name[0] = 0xE5;
                }
                rc = embk_block_write(vol->dev, first_sector,
                                      sectors_per_cluster, cluster_buffer);
                if (rc != EMBK_OK) {
                    return rc;
                }
                run_len = 0;
                break;
            }

            if (marker == 0xE5) {
                if (run_len == 0) {
                    run_start = i;
                }
                run_len++;
                if (run_len >= needed_slots) {
                    *out_cluster = cluster;
                    *out_index = run_start;
                    return EMBK_OK;
                }
                continue;
            }

            run_len = 0;
        }

        uint32_t next = fat_get_next_cluster(vol, cluster);
        if (next == 0) {
            return -EMBK_EIO;
        }
        cluster = next;
    }

    // No usable run in the existing chain: append a fresh, zeroed cluster.
    uint32_t new_cluster;
    int rc = fat32_alloc_cluster_chain(vol, 1, &new_cluster, NULL);
    if (rc != EMBK_OK) {
        return rc;
    }

    memset(cluster_buffer, 0, cluster_size);
    uint32_t new_first_sector = cluster_to_sector(vol, new_cluster);
    rc = embk_block_write(vol->dev, new_first_sector,
                          sectors_per_cluster, cluster_buffer);
    if (rc != EMBK_OK) {
        fat32_free_cluster_chain(vol, new_cluster);
        return rc;
    }

    if (last_cluster != 0) {
        rc = fat32_write_fat_entry(vol, last_cluster, new_cluster);
        if (rc != EMBK_OK) {
            fat32_free_cluster_chain(vol, new_cluster);
            return rc;
        }
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

        char component[256];
        if (len >= sizeof(component)) {
            return -EMBK_EINVAL;
        }

        if (*p == '\0') {
            memcpy(out_name, segment, len);
            out_name[len] = '\0';
            *out_parent_cluster = cluster;
            return EMBK_OK;
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
    uint32_t sectors_per_cluster = vol->sectors_per_cluster;
    uint32_t cluster_size        = vol->bytes_per_sector * sectors_per_cluster;
    uint32_t entries_per_cluster = cluster_size / sizeof(struct fat_dir_entry);

    static uint8_t dir_buffer[4096] __attribute__((aligned(4)));
    if (cluster_size > sizeof(dir_buffer)) return -EMBK_EINVAL;

    struct fat32_lfn_entry lfn_entries[20];
    uint32_t lfn_count = 0;

    uint32_t cluster = dir_cluster;
    while (cluster >= 2 && cluster < FAT32_EOC_MIN && cluster != FAT32_FREE) {
        uint32_t first_sector = cluster_to_sector(vol, cluster);
        int rc = embk_block_read(vol->dev, first_sector, sectors_per_cluster, dir_buffer);
        if (rc != EMBK_OK) return rc;

        struct fat_dir_entry *entries = (struct fat_dir_entry *)dir_buffer;
        for (uint32_t i = 0; i < entries_per_cluster; i++) {
            struct fat_dir_entry *entry = &entries[i];

            if (entry->name[0] == 0x00) return -EMBK_ENOENT;   // end of dir, not found
            if (entry->name[0] == 0xE5) {
                lfn_count = 0;
                continue;              // deleted
            }
            if ((entry->attr & FAT32_ATTR_LFN) == FAT32_ATTR_LFN) {
                if (lfn_count < sizeof(lfn_entries) / sizeof(lfn_entries[0])) {
                    lfn_entries[lfn_count++] = *(struct fat32_lfn_entry *)entry;
                }
                continue;
            }
            if (entry->attr & FAT32_ATTR_VOLUME_ID) {
                lfn_count = 0;
                continue;
            }

            if (fat32_match_dir_entry_name(name, entry, lfn_entries, lfn_count)) {
                *out = *entry;
                return EMBK_OK;
            }
            lfn_count = 0;
        }

        cluster = fat_get_next_cluster(vol, cluster);
        if (cluster == 0) return -EMBK_EIO;   // broken chain
    }
    return -EMBK_ENOENT;
}

// FIX #3: write the LFN entries in correct on-disk order and suppress the LFN
// set entirely for short names.
//
// FAT stores LFN slots in REVERSE: the highest sequence number (OR'd with the
// 0x40 "last entry" flag) comes first on disk, sequence 1 comes last, directly
// before the 8.3 short entry. The old code wrote them ascending, which the
// reconstruct routine tolerates (it sorts) but Linux/mdir/Windows do not.
//
// The old code also always emitted at least one LFN slot, attaching a stray
// LFN to plain 8.3 names whose slot count disagreed with needed_slots.
static int fat32_write_dir_entry_with_lfn(struct fat32_volume *vol,
                                          uint8_t *cluster_buffer,
                                          uint32_t entries_per_cluster,
                                          uint32_t entry_index,
                                          const char *name,
                                          const struct fat_dir_entry *template_entry) {
    (void)vol;

    // Short names: a single 8.3 entry, no LFN.
    if (fat32_name_is_short(name)) {
        struct fat_dir_entry *short_entry =
            (struct fat_dir_entry *)&cluster_buffer[entry_index * sizeof(struct fat_dir_entry)];
        *short_entry = *template_entry;
        return EMBK_OK;
    }

    uint16_t name_chars[260];
    uint32_t char_count = fat32_encode_lfn_name(name, name_chars,
                                                sizeof(name_chars) / sizeof(name_chars[0]));
    uint32_t lfn_slots = (char_count + 12) / 13;

    if (entry_index + lfn_slots >= entries_per_cluster) {
        return -EMBK_EINVAL;
    }

    uint8_t checksum = fat32_short_name_checksum(template_entry->name);

    // Reverse order on disk: highest sequence (with 0x40 flag) at the lowest
    // disk index, sequence 1 immediately before the short entry.
    for (uint32_t slot = 0; slot < lfn_slots; slot++) {
        uint32_t seq = lfn_slots - slot;              // 1-based, descending
        uint32_t index = entry_index + slot;
        struct fat32_lfn_entry *lfn =
            (struct fat32_lfn_entry *)&cluster_buffer[index * sizeof(struct fat_dir_entry)];

        uint8_t order = (uint8_t)seq;
        if (slot == 0) {
            order |= 0x40;                            // "last LFN entry" flag
        }

        uint32_t name_offset = (seq - 1) * 13;
        uint32_t remaining = (char_count > name_offset) ? (char_count - name_offset) : 0;
        fat32_fill_lfn_entry(lfn, order, checksum, name_chars, name_offset, remaining);
    }

    struct fat_dir_entry *short_entry =
        (struct fat_dir_entry *)&cluster_buffer[(entry_index + lfn_slots) * sizeof(struct fat_dir_entry)];
    *short_entry = *template_entry;
    return EMBK_OK;
}

int fat32_mkdir(struct fat32_volume *vol, const char *path) {
    if (!vol || !vol->mounted || !path || !*path) {
        return -EMBK_EINVAL;
    }

    uint32_t parent_cluster;
    char dirname[256];
    int rc = fat32_find_parent_dir(vol, path, &parent_cluster, dirname);
    if (rc != EMBK_OK) {
        return rc;
    }

    uint32_t entry_cluster = 0;
    uint32_t entry_index = 0;
    struct fat_dir_entry existing_entry;
    rc = fat32_find_dir_entry_location(vol, parent_cluster, dirname,
                                       &entry_cluster, &entry_index,
                                       &existing_entry);
    if (rc == EMBK_OK) {
        return -EMBK_EEXIST;
    }
    if (rc != -EMBK_ENOENT) {
        return rc;
    }

    uint32_t new_cluster;
    rc = fat32_alloc_cluster_chain(vol, 1, &new_cluster, NULL);
    if (rc != EMBK_OK) {
        return rc;
    }

    uint32_t lfn_slots = fat32_lfn_slot_count(dirname);
    uint32_t needed_slots = lfn_slots + 1;

    uint32_t dir_entry_cluster;
    uint32_t dir_entry_index;
    uint32_t cluster_size = vol->bytes_per_sector * vol->sectors_per_cluster;
    uint8_t *cluster_buffer = kmalloc(cluster_size);
    if (!cluster_buffer) {
        fat32_free_cluster_chain(vol, new_cluster);
        return -EMBK_ENOMEM;
    }

    rc = fat32_find_free_dir_slot(vol, parent_cluster, needed_slots,
                                  &dir_entry_cluster, &dir_entry_index,
                                  cluster_buffer);
    if (rc != EMBK_OK) {
        kfree(cluster_buffer);
        fat32_free_cluster_chain(vol, new_cluster);
        return rc;
    }

    uint32_t first_sector = cluster_to_sector(vol, dir_entry_cluster);
    rc = embk_block_read(vol->dev, first_sector,
                         vol->sectors_per_cluster, cluster_buffer);
    if (rc != EMBK_OK) {
        kfree(cluster_buffer);
        fat32_free_cluster_chain(vol, new_cluster);
        return rc;
    }

    struct fat_dir_entry template_entry;
    memset(&template_entry, 0, sizeof(template_entry));
    fat32_generate_short_name(dirname, template_entry.name);
    template_entry.attr = FAT32_ATTR_DIRECTORY;
    template_entry.create_date = FAT32_EPOCH_DATE;   // FIX #7: valid date
    template_entry.write_date  = FAT32_EPOCH_DATE;
    template_entry.access_date = FAT32_EPOCH_DATE;
    template_entry.first_cluster_high = (uint16_t)(new_cluster >> 16);
    template_entry.first_cluster_low = (uint16_t)(new_cluster & 0xFFFF);
    template_entry.file_size = 0;

    rc = fat32_write_dir_entry_with_lfn(vol, cluster_buffer,
                                        cluster_size / sizeof(struct fat_dir_entry),
                                        dir_entry_index, dirname, &template_entry);
    if (rc != EMBK_OK) {
        kfree(cluster_buffer);
        fat32_free_cluster_chain(vol, new_cluster);
        return rc;
    }

    rc = embk_block_write(vol->dev, first_sector,
                          vol->sectors_per_cluster, cluster_buffer);
    if (rc != EMBK_OK) {
        kfree(cluster_buffer);
        fat32_free_cluster_chain(vol, new_cluster);
        return rc;
    }

    memset(cluster_buffer, 0, cluster_size);
    struct fat_dir_entry *dot_entries = (struct fat_dir_entry *)cluster_buffer;
    memset(dot_entries, 0, sizeof(struct fat_dir_entry) * 2);
    memcpy(dot_entries[0].name, ".          ", 11);
    dot_entries[0].attr = FAT32_ATTR_DIRECTORY;
    dot_entries[0].create_date = FAT32_EPOCH_DATE;
    dot_entries[0].write_date  = FAT32_EPOCH_DATE;
    dot_entries[0].access_date = FAT32_EPOCH_DATE;
    dot_entries[0].first_cluster_high = (uint16_t)(new_cluster >> 16);
    dot_entries[0].first_cluster_low = (uint16_t)(new_cluster & 0xFFFF);

    // FIX #6: the '..' entry of a directory directly under the root must hold
    // cluster 0, NOT the root's real cluster number (2 here). FAT12/16 had no
    // cluster number for the root; FAT32 keeps the convention and fsck enforces
    // it. For a deeper nesting, parent_cluster != root_cluster so '..' points
    // at the real parent as expected.
    uint32_t dotdot_cluster =
        (parent_cluster == vol->root_cluster) ? 0 : parent_cluster;

    memcpy(dot_entries[1].name, "..         ", 11);
    dot_entries[1].attr = FAT32_ATTR_DIRECTORY;
    dot_entries[1].create_date = FAT32_EPOCH_DATE;
    dot_entries[1].write_date  = FAT32_EPOCH_DATE;
    dot_entries[1].access_date = FAT32_EPOCH_DATE;
    dot_entries[1].first_cluster_high = (uint16_t)(dotdot_cluster >> 16);
    dot_entries[1].first_cluster_low = (uint16_t)(dotdot_cluster & 0xFFFF);

    uint32_t new_dir_first_sector = cluster_to_sector(vol, new_cluster);
    rc = embk_block_write(vol->dev, new_dir_first_sector,
                          vol->sectors_per_cluster, cluster_buffer);
    kfree(cluster_buffer);
    if (rc != EMBK_OK) {
        fat32_free_cluster_chain(vol, new_cluster);
        return rc;
    }

    return EMBK_OK;
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

    // FIX #5: when overwriting an existing file, the entry may live in a later
    // cluster of a multi-cluster directory. fat32_find_dir_entry_location gives
    // us entry_cluster/entry_index relative to that cluster, so write the update
    // there — not blindly into parent_cluster (which corrupted unrelated slots).
    uint32_t dir_cluster;
    if (!exists) {
        uint32_t needed_slots = fat32_lfn_slot_count(filename) + 1;
        rc = fat32_find_free_dir_slot(vol, parent_cluster, needed_slots,
                                      &dir_cluster, &entry_index,
                                      cluster_buffer);
        if (rc != EMBK_OK) {
            kfree(cluster_buffer);
            if (new_head) {
                fat32_free_cluster_chain(vol, new_head);
            }
            return rc;
        }
    } else {
        dir_cluster = entry_cluster;   // entry may be in a later cluster
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
        struct fat_dir_entry template_entry;
        memset(&template_entry, 0, sizeof(template_entry));
        fat32_generate_short_name(filename, template_entry.name);
        template_entry.attr = FAT32_ATTR_ARCHIVE;
        template_entry.create_date = FAT32_EPOCH_DATE;   // FIX #7: valid date
        template_entry.write_date  = FAT32_EPOCH_DATE;
        template_entry.access_date = FAT32_EPOCH_DATE;
        template_entry.first_cluster_high = (uint16_t)(new_head >> 16);
        template_entry.first_cluster_low = (uint16_t)(new_head & 0xFFFF);
        template_entry.file_size = size;

        rc = fat32_write_dir_entry_with_lfn(vol, cluster_buffer,
                                            cluster_size / sizeof(struct fat_dir_entry),
                                            entry_index, filename, &template_entry);
        if (rc != EMBK_OK) {
            kfree(cluster_buffer);
            if (new_head) {
                fat32_free_cluster_chain(vol, new_head);
            }
            return rc;
        }
    } else {
        entries[entry_index].first_cluster_high = (uint16_t)(new_head >> 16);
        entries[entry_index].first_cluster_low = (uint16_t)(new_head & 0xFFFF);
        entries[entry_index].file_size = size;
    }

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