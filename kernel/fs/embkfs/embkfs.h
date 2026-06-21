#ifndef _EMBKFS_H
#define _EMBKFS_H

#include <stdint.h>

/* =====================================================================
 * EMBKFS on-disk format — v2.0
 * Every struct mirrors the spec byte-for-byte. The _Static_assert after
 * each one is the contract: off by a single byte and the build fails,
 * instead of silently misreading the disk.
 * ===================================================================== */

/* Magic numbers (little-endian on disk -> readable ASCII in a hex dump) */
#define EMBKFS_MAGIC       0x373153464B424D45ULL  /* "EMBKFS17" */
#define EMBKFS_NODE_MAGIC  0x45444F4E4B424D45ULL  /* "EMBKNODE" */

/* Primary superblock at a fixed *byte* offset (block size unknown yet) */
#define EMBKFS_SB_OFFSET   65536                  /* 64 KiB */

/* Item types (key.type). Gaps left for future insertion in sort order. */
#define EMBK_TYPE_INODE      1
#define EMBK_TYPE_DIR_ENTRY  16
#define EMBK_TYPE_EXTENT     32
#define EMBK_TYPE_XATTR      48

/* ---- Addressing primitives ---------------------------------------- */

/* §6  Fat pointer: locate + verify + provenance. 32 B -> 128 per 4 KiB block. */
struct embk_block_ptr {
    uint64_t block;        /*  0  target block number on disk            */
    uint64_t checksum;     /*  8  CRC32C of target contents (low 32, v1) */
    uint64_t generation;   /* 16  generation that wrote the target       */
    uint64_t flags;        /* 24  reserved in v1, must be 0              */
} __attribute__((packed));
_Static_assert(sizeof(struct embk_block_ptr) == 32, "block_ptr must be 32 bytes");

/* §7  Composite key. Compared object_id, then type, then offset. */
struct embk_key {
    uint64_t object_id;    /*  0  which object this record belongs to    */
    uint64_t type;         /*  8  record kind (1 byte used, upper 7 = 0) */
    uint64_t offset;       /* 16  polymorphic, interpreted per 'type'    */
} __attribute__((packed));
_Static_assert(sizeof(struct embk_key) == 24, "key must be 24 bytes");

/* ---- Superblock (§5.2) -------------------------------------------- */
struct embk_superblock {
    uint64_t magic;             /*   0 */
    uint32_t version_major;     /*   8 */
    uint32_t version_minor;     /*  12 */
    uint64_t feature_compat;    /*  16 */
    uint64_t feature_ro_compat; /*  24 */
    uint64_t feature_incompat;  /*  32 */
    uint64_t block_size;        /*  40 */
    uint64_t total_blocks;      /*  48 */
    uint64_t free_blocks;       /*  56 */
    uint8_t  uuid[16];          /*  64 */
    uint64_t generation;        /*  80 */
    struct embk_block_ptr root;       /*  88 */
    struct embk_block_ptr checkpoint; /* 120 */
    uint64_t checksum;          /* 152  covers all preceding bytes */
} __attribute__((packed));
_Static_assert(sizeof(struct embk_superblock) == 160, "superblock must be 160 bytes");

/* ---- Tree nodes (§8) ---------------------------------------------- */

/* §8.1  Common header. checksum at offset 0 covers [8 .. block_size-1]. */
struct embk_node_header {
    uint64_t checksum;     /*  0  CRC32C over [8 .. block_size-1]        */
    uint64_t magic;        /*  8  EMBKFS_NODE_MAGIC                      */
    uint64_t generation;   /* 16  cross-checked against the pointer      */
    uint64_t block;        /* 24  cross-checked against the pointer      */
    uint8_t  level;        /* 32  0 = leaf, >0 = height above leaves     */
    uint8_t  reserved[3];  /* 33  must be 0                             */
    uint32_t nritems;      /* 36  items currently in this node           */
} __attribute__((packed));
_Static_assert(sizeof(struct embk_node_header) == 40, "node header must be 40 bytes");

/* §8.2  Internal node slot: {smallest key in subtree, pointer to child}. */
struct embk_internal_slot {
    struct embk_key       key;  /*  0 */
    struct embk_block_ptr ptr;  /* 24 */
} __attribute__((packed));
_Static_assert(sizeof(struct embk_internal_slot) == 56, "internal slot must be 56 bytes");

/* §8.3  Leaf item header (slotted page: headers grow from front). */
struct embk_item_header {
    struct embk_key key;   /*  0  leaf array stays sorted by this        */
    uint32_t offset;       /* 24  data location, from START of block     */
    uint32_t size;         /* 28  data length in bytes                   */
} __attribute__((packed));
_Static_assert(sizeof(struct embk_item_header) == 32, "item header must be 32 bytes");

/* ---- Item types (§9) ---------------------------------------------- */

/* §9.1  Inode. NOTE: reserved is 48, not 40 — real fields = 80, 80+48 = 128. */
struct embk_inode_item {
    uint64_t size;         /*  0  object size in bytes                   */
    uint64_t blocks;       /*  8  blocks allocated (sparse detection)    */
    uint64_t links;        /* 16  hard-link count                        */
    uint32_t mode;         /* 24  POSIX st_mode                          */
    uint32_t uid;          /* 28 */
    uint32_t gid;          /* 32 */
    uint32_t flags;        /* 36  per-object flags (reserved v1)         */
    uint64_t atime;        /* 40  ns since Unix epoch                    */
    uint64_t mtime;        /* 48 */
    uint64_t ctime;        /* 56 */
    uint64_t btime;        /* 64 */
    uint64_t generation;   /* 72 */
    uint8_t  reserved[48]; /* 80  must be 0                             */
} __attribute__((packed));
_Static_assert(sizeof(struct embk_inode_item) == 128, "inode must be 128 bytes");

/* §9.2  Directory entry. Fixed part 16 B, then name_len UTF-8 bytes. */
struct embk_dir_entry_item {
    uint64_t target_object_id; /* 0  object this name refers to          */
    uint8_t  target_type;      /* 8  file/dir/symlink (dup'd from target) */
    uint8_t  name_len;         /* 9  1..255 (POSIX NAME_MAX)             */
    uint8_t  reserved[6];      /* 10 must be 0                          */
    /* followed by name_len bytes of UTF-8, NOT null-terminated */
} __attribute__((packed));
_Static_assert(sizeof(struct embk_dir_entry_item) == 16, "dir entry fixed part must be 16 bytes");

/* §9.3  Extent: a contiguous run of file data + its own data checksum. */
struct embk_extent_item {
    uint64_t disk_block;    /*  0  first disk block of the run           */
    uint64_t length;        /*  8  run length in blocks                  */
    uint64_t logical_size;  /* 16  actual bytes (last block may partial) */
    uint64_t checksum;      /* 24  CRC32C over the extent's DATA         */
    uint64_t generation;    /* 32 */
    uint32_t flags;         /* 40  HOLE/COMPRESSED/ENCRYPTED (reserved)  */
    uint32_t reserved0;     /* 44  must be 0                            */
    uint8_t  reserved1[16]; /* 48  must be 0                            */
} __attribute__((packed));
_Static_assert(sizeof(struct embk_extent_item) == 64, "extent must be 64 bytes");

/* ---- Public API (grows over the next steps) ----------------------- */
void embkfs_init(void);

#endif /* _EMBKFS_H */