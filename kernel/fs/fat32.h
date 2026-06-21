#ifndef __FAST32_H__
#define __FAST32_H__


#include "../include/types.h"
#include "../block/block.h"

#include <stdint.h>

// ===============================================================================
//  FAT32 Boot Sector / BIOS Parameter Block (on-disk layout)
// ===============================================================================
// This struct is overlaid directly onto the 512 bytes of sector 0.
// __attribute__((packed)) is MANDATORY - the compiler must not insert
// any padding, or the field offsets won't match the on-disk layout.
// Offsets in comments are bytes from the start of the sector.

struct fat32_bpb {
    // --- Common BPB (shared by FAT12 and FAT16) ---
    uint8_t  jmp_boot[3];                   // 0x00: 3-byte jump instruction
    uint8_t  oem_name[8];                   // 0x03: "mkfs.fat" etc., ignore
    uint16_t bytes_per_sector;              // 0x0B: bytes/sector, expert 512
    uint8_t  sectors_per_cluster;           // 0x0D: sectors/cluster,expert 1-63
    uint16_t reserved_sectors;              // 0x0E: expect 32 (FAT region start here)
    uint8_t  num_fats;                      // 0x10: number of FATs, expect 2
    uint16_t root_entries_16;               // 0x11: FAT12/16 only - 0 on FAT32
    uint16_t total_sectors_16;              // 0x13: FAT12/16 only - 0 on FAT32 (see _32)
    uint8_t  media_type;                    // 0x15: 0xF8 for fixed disk, 0xF0 for removable
    uint16_t fat_size_16;                   // 0x16: FAT12/16 only - 0 on FAT32  (USE _32)
    uint16_t sectors_per_track;             // 0x18: geometry, ignore
    uint16_t num_heads;                     // 0x1A: geometry, ignore
    uint32_t hidden_sectors;                // 0x1C: sectors before this partition
    uint32_t total_sectors_32;              // 0x1E: total sectors on disk 

    // --- FAT32-specific extention (offset 0x24 onward) ---
    uint32_t fat_size_32;                   // 0x24: sectors per FAT (expect 1009)
    uint16_t ext_flags;                     // 0x28: flags (FAT mirroring, etc.)
    uint16_t fs_version;                    // 0x2A: version
    uint32_t root_cluster;                  // 0x2C: first cluster of root directory
    uint16_t fs_info_sector;                // 0x30: FSinfo sector number
    uint16_t backup_boot_sector;            // 0x32: backup boot sector number
    uint8_t  reserved[12];                  // 0x34: reserved, zero
    uint8_t  drive_number;                  // 0x40: BIOS drive number
    uint8_t  reserved1;                     // 0x41: reserved
    uint8_t  boot_signature;                // 0x42: boot signature (0x29) IF next 3 fields are present
    uint32_t volume_id;                     // 0x43: volume serial number
    uint8_t  volume_label[11];              // 0x47: "EMBLINK   " (space-padded)
    uint8_t  fs_type[8];                    // 0x52: "FAT32   " (NOT reliable, just hint)

    // (boot code + 0x55AA signature fill the rest of the 512 bytes)
}  __attribute__((packed));

// ===================================================================================================
//  Mounted-volume state (computed from the BPB, kept in memory)
// ===================================================================================================
// After parsing the BPB, we precompute the region location so we
// don't have to do it every time we read/write a sector.

struct fat32_volume {
    struct embk_block_device *dev;          // device we're mounted on

    // copied straight from the BPB
    uint16_t bytes_per_sector;              
    uint8_t  sectors_per_cluster;           
    uint32_t root_cluster;     
    uint32_t total_sectors;    
    
    // computed region locations (in absolute sector numbers)
    uint32_t fat_start_sector;             // = reserved_sectors
    uint32_t data_start_sector;            // = reserved_sectors + (num_fats * fat_size_32)
    uint32_t sectors_per_fat;               // = fat_size_32
    uint8_t  num_fats;                     

    bool     mounted;                       // true once successfully parsed
};

// A 32-byte FAT directory entry (on-disk layout)
struct fat_dir_entry {
    uint8_t  name[11];            // 0x00  8.3 name, space-padded
    uint8_t  attr;                // 0x0B  attribute bits
    uint8_t  nt_reserved;         // 0x0C  reserved for Windows NT
    uint8_t  create_time_tenth;   // 0x0D  creation time, tenths of a second
    uint16_t create_time;         // 0x0E  creation time
    uint16_t create_date;         // 0x10  creation date
    uint16_t access_date;         // 0x12  last access date
    uint16_t first_cluster_high;  // 0x14  high 16 bits of first cluster
    uint16_t write_time;          // 0x16  last write time
    uint16_t write_date;          // 0x18  last write date
    uint16_t first_cluster_low;   // 0x1A  low 16 bits of first cluster
    uint32_t file_size;           // 0x1C  file size in bytes
} __attribute__((packed));        // total = 32 bytes


// FAT entries special values (mask off the top 4 bits - they're reserved))  
#define FAT32_EOC_MIN        0x0FFFFFF8     // >= this means end of chain
#define FAT32_BAD            0xFFFFFFF7     // bad cluster
#define FAT32_FREE           0x00000000     // free cluster
#define FAT32_ENTRY_MASK     0x0FFFFFFF     // ONLY low 28 bits are cluster number

// Directory entry attribute bits
#define FAT32_ATTR_READ_ONLY 0x01
#define FAT32_ATTR_HIDDEN    0x02
#define FAT32_ATTR_SYSTEM    0x04
#define FAT32_ATTR_VOLUME_ID 0x08
#define FAT32_ATTR_DIRECTORY 0x10
#define FAT32_ATTR_ARCHIVE   0x20
#define FAT32_ATTR_LFN       0x0F           // long file name (LFN) bit


// Mount a FAT32 volume from a block device: read sectors 0, parse the
// BPB, and compute the region locations, validate.  Returns 0 on success.
// negative EMBK_* error on failure. Fills *vol.
int fat32_mount(struct embk_block_device *dev, struct fat32_volume *vol);

// List the root directory entries on a mounted FAT32 volume.
void fat32_list_root(struct fat32_volume *vol);

int fat32_read(struct fat32_volume *vol, const char *name, uint8_t *buffer, uint32_t max_size);
int fat32_write(struct fat32_volume *vol, const char *name, const uint8_t *buffer, uint32_t size);
int fat32_mkdir(struct fat32_volume *vol, const char *path);

static uint32_t fat32_cluster_count(struct fat32_volume *vol);
static bool fat32_valid_cluster(struct fat32_volume *vol, uint32_t cluster);
static int fat32_read_fat_entry(struct fat32_volume *vol, uint32_t cluster,
                                uint32_t *out_value);
static int fat32_write_fat_entry(struct fat32_volume *vol, uint32_t cluster,
                                 uint32_t value);
static int fat32_free_cluster_chain(struct fat32_volume *vol, uint32_t cluster);
static int fat32_alloc_cluster_chain(struct fat32_volume *vol, uint32_t count,
                                     uint32_t *out_head, uint32_t *out_tail);
static int fat32_find_dir_entry_location(struct fat32_volume *vol,
                                        uint32_t dir_cluster,
                                        const char *name,
                                        uint32_t *out_cluster,
                                        uint32_t *out_index,
                                        struct fat_dir_entry *out_entry);
static int fat32_find_free_dir_slot(struct fat32_volume *vol,
                                    uint32_t dir_cluster,
                                    uint32_t needed_slots,
                                    uint32_t *out_cluster,
                                    uint32_t *out_index,
                                    uint8_t *cluster_buffer);
static int fat32_find_parent_dir(struct fat32_volume *vol,
                                 const char *path,
                                 uint32_t *out_parent_cluster,
                                 char *out_name);
static int fat32_find_path(struct fat32_volume *vol, const char *path,
                           struct fat_dir_entry *out_entry);
static int fat32_find_in_dir(struct fat32_volume *vol, uint32_t dir_cluster,
                             const char *name, struct fat_dir_entry *out);
static int fat32_write_data_to_chain(struct fat32_volume *vol,
                                     uint32_t start_cluster,
                                     const uint8_t *buffer, uint32_t size);

#endif /*__FAST32_H__*/ 