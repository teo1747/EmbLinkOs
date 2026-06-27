#ifndef _EMBKFS_H
#define _EMBKFS_H

#include "../../include/types.h"   /* bool, NULL, size_t */
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


/* ---- Mount-time constants (read-only, v2.2) ------------------------- */

/* The superblock checksum covers every byte BEFORE the trailing 8-byte
 * checksum field. Deriving it from the struct (total minus that final u64)
 * keeps it from ever drifting from the layout: 160 - 8 = 152. */
#define EMBKFS_SB_BODY_SIZE   (sizeof(struct embk_superblock) - sizeof(uint64_t))

/* Feature bits this reader understands — none yet. So any set incompat bit
 * means refuse; any set ro_compat bit means mount read-only. */
#define EMBKFS_KNOWN_INCOMPAT   0ULL
#define EMBKFS_KNOWN_RO_COMPAT  0ULL

/* Highest major version we know how to read. */
#define EMBKFS_MAX_KNOWN_MAJOR  1u

/* ---- Filesystem object conventions -------------------------------- */

/* The root directory is always object id 1 — a fixed entry point into the
 * namespace, the same idea as the superblock's fixed offset into the volume. */
#define EMBKFS_ROOT_OBJECT_ID   1ULL

/* Object ids 0 and 1 are reserved (0 = none/null, 1 = root dir); the first id a
 * create may hand out is 2. The allocator scans the live tree at mount for the
 * highest id in use and continues from there (see embkfs_volume.next_oid). */
#define EMBKFS_FIRST_USER_OBJID 2ULL

/* POSIX st_mode helpers (embk_inode_item.mode). */
#define EMBKFS_S_IFMT    0170000u   /* file-type mask */
#define EMBKFS_S_IFDIR   0040000u   /* directory      */
#define EMBKFS_S_IFREG   0100000u   /* regular file   */
#define EMBKFS_S_IFLNK   0120000u   /* symlink        */

/* Default permission bits for objects this implementation creates. */
#define EMBKFS_PERM_FILE 0644u
#define EMBKFS_PERM_DIR  0755u
#define EMBKFS_PERM_LNK  0777u

/* seek(whence) values for embkfs_seek_object. */
#define EMBKFS_SEEK_SET  0
#define EMBKFS_SEEK_CUR  1
#define EMBKFS_SEEK_END  2

/* embk_dir_entry_item.target_type — the file kind, duplicated from the target
 * inode so a directory listing needn't read every target (spec §9.2). */
#define EMBKFS_DT_REG    1u   /* regular file */
#define EMBKFS_DT_DIR    2u   /* directory    */
#define EMBKFS_DT_LNK    3u   /* symlink      */


/* (block_size - node_header) / internal_slot = (4096 - 40) / 56 = 72 */
#define EMBKFS_MAX_SLOTS  72



#define EMBK_TXN_MAX  64    /* per-block node slots: a commit rewrites a
                             * tree-height-sized handful of nodes              */
#define EMBK_TXN_RUNS 16    /* data-run slots: distinct extents one write may
                             * allocate or supersede. Caps file fragmentation a
                             * single write can absorb (see embkfs_write_file). */


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

#define EMBKFS_EXTENT_F_HOLE 0x00000001u


/* ---- In-memory mount state ---------------------------------------- */

struct embk_block_device;   /* forward decl — we only keep a pointer */
struct embk_txn;            /* the in-flight COW transaction (embkfs.c-private) */
struct embk_run;

struct embkfs_volume {
    struct embk_block_device *dev;   /* device we're mounted on        */
    uint64_t block_size;             /* filesystem block size (bytes)  */
    uint64_t total_blocks;           /* volume size in blocks          */
    uint64_t generation;             /* superblock generation          */
    uint64_t free_blocks;            /* read from the superblock at mount (next to total_blocks) */
    uint64_t next_oid;               /* next object id to hand out (max in tree + 1, set at mount) */
    uint8_t *block_bitmap;           /* 1 bit per block: 1 = used, 0 = free */
    struct embk_run *free_ext;       /* sorted/coalesced free runs built from bitmap */
    uint32_t free_ext_n;
    uint32_t free_ext_cap;
    struct embk_txn *txn;            /* set while a write op is in flight: blocks it allocates /
                                      * supersedes are recorded here, reconciled into the bitmap
                                      * once the commit is durable (NULL outside a transaction) */

    struct embk_block_ptr root;      /* pointer into the metadata tree */
    bool     read_only;              /* forced RO by an ro_compat bit  */
    bool     mounted;                /* true once the SB validated     */
};


/* COW rebuild helpers used by the metadata update path. */

/* A single operation for the COW engine, keyed by `key`:
 *   - PUT (del == false): set the item to `data`/`size` (insert, or replace if
 *     the key already exists). Every metadata write is a put — an inode, a
 *     directory entry, an extent — the engine never interprets the bytes.
 *   - DELETE (del == true): remove the item with this key; `data`/`size` are
 *     ignored. Deleting an absent key is a no-op (idempotent).
 * Existing put-builders need no change: designated initializers zero `del`. */
struct embk_put {
    struct embk_key key;
    const uint8_t *data;
    uint32_t size;
    bool del;
};

struct embk_child {
    struct embk_key key;
    struct embk_block_ptr ptr;
};

struct embk_litem {
    struct embk_key key;
    const uint8_t *data;
    uint32_t size;
};


/* A contiguous run of blocks: [start .. start+len). */
struct embk_run { uint64_t start; uint64_t len; };
 
struct embk_txn {
    uint64_t alloc[EMBK_TXN_MAX]; uint32_t alloc_n;        /* node blocks allocated  */
    uint64_t freed[EMBK_TXN_MAX]; uint32_t freed_n;        /* node blocks superseded */
    struct embk_run *arun; uint32_t arun_n; uint32_t arun_cap; /* data runs allocated    */
    struct embk_run *frun; uint32_t frun_n; uint32_t frun_cap; /* data runs superseded   */
    bool     overflow;                                     /* any list hit its bound */
};


/* ---- Public API (grows over the next steps) ----------------------- */
void embkfs_init(void);

/* Probe one block device: read + verify the superblock at byte 65536, and on
 * success fill *vol. Returns EMBK_OK, or -EMBK_EINVAL if the device isn't an
 * EMBKFS volume (or its superblock is corrupt). */
int embkfs_mount(struct embk_block_device *dev, struct embkfs_volume *vol);

/* Read the block `ptr` targets and verify it as a tree node, against `ptr`
 * itself. On success `buf` holds the whole block_size-byte block. Used for
 * every node in the tree. buf_size must be >= the volume's block_size. */
int embkfs_read_node(struct embkfs_volume *vol,
                     const struct embk_block_ptr *ptr,
                     uint8_t *buf, size_t buf_size);

/* Resolve one name inside a directory to its target object id. */
int embkfs_lookup_name(struct embkfs_volume *vol, uint64_t dir_oid,
                       const char *name, uint64_t *out_oid);
int embkfs_lookup_path(struct embkfs_volume *vol, uint64_t start_dir_oid,
                       const char *path, uint64_t *out_oid);
int embkfs_normalize_path(const char *path, char *out, size_t out_sz);
int embkfs_run_path_selftests(void);
int embkfs_run_allocator_selftests(void);
int embkfs_run_tree_selftests(void);
int embkfs_run_object_selftests(void);
int embkfs_run_namespace_selftests(void);

/* Namespace/data mutators. */
int embkfs_create_file(struct embkfs_volume *vol, uint64_t dir_oid,
                       const char *name, uint64_t *out_oid);
int embkfs_create_file_path(struct embkfs_volume *vol, uint64_t start_dir_oid,
                            const char *path, uint64_t *out_oid);
int embkfs_mkdir_name(struct embkfs_volume *vol, uint64_t dir_oid,
                      const char *name, uint64_t *out_oid);
int embkfs_mkdir_path(struct embkfs_volume *vol, uint64_t start_dir_oid,
                      const char *path, uint64_t *out_oid);
int embkfs_write_object(struct embkfs_volume *vol, uint64_t oid,
                        const uint8_t *data, uint64_t len);
int embkfs_read_object(struct embkfs_volume *vol, uint64_t oid,
                       uint8_t *buf, uint64_t buf_sz,
                       uint64_t *out_read);
int embkfs_read_object_at(struct embkfs_volume *vol, uint64_t oid,
                          uint64_t offset, uint8_t *buf, uint64_t len,
                          uint64_t *out_read);
int embkfs_write_object_at(struct embkfs_volume *vol, uint64_t oid,
                           uint64_t offset, const uint8_t *data, uint64_t len,
                           uint64_t *out_written);
int embkfs_append_object(struct embkfs_volume *vol, uint64_t oid,
                         const uint8_t *data, uint64_t len,
                         uint64_t *out_written);
int embkfs_seek_object(struct embkfs_volume *vol, uint64_t oid,
                       int64_t current_offset, int whence, int64_t delta,
                       uint64_t *out_offset);
int embkfs_truncate_object(struct embkfs_volume *vol, uint64_t oid,
                           uint64_t new_size);
int embkfs_resize_object(struct embkfs_volume *vol, uint64_t oid,
                         uint64_t new_size);
int embkfs_unlink_name(struct embkfs_volume *vol, uint64_t dir_oid,
                       const char *name);
int embkfs_unlink_path(struct embkfs_volume *vol, uint64_t start_dir_oid,
                       const char *path);
int embkfs_remove_entry_name(struct embkfs_volume *vol, uint64_t dir_oid,
                             const char *name);
int embkfs_remove_entry_path(struct embkfs_volume *vol, uint64_t start_dir_oid,
                             const char *path);
int embkfs_rmdir_name(struct embkfs_volume *vol, uint64_t dir_oid,
                      const char *name);
int embkfs_rmdir_path(struct embkfs_volume *vol, uint64_t start_dir_oid,
                      const char *path);

/* Rename/move an entry from (old_dir, old_name) to (new_dir, new_name).
 * Fails with -EMBK_EEXIST if destination already exists. */
int embkfs_rename_name(struct embkfs_volume *vol,
                       uint64_t old_dir_oid, const char *old_name,
                       uint64_t new_dir_oid, const char *new_name);
int embkfs_rename_path(struct embkfs_volume *vol, uint64_t start_dir_oid,
                       const char *old_path, const char *new_path);
int embkfs_link_name(struct embkfs_volume *vol, uint64_t target_oid,
                     uint64_t new_dir_oid, const char *new_name);
int embkfs_link_path(struct embkfs_volume *vol, uint64_t start_dir_oid,
                     const char *target_path, const char *new_path);
int embkfs_symlink_name(struct embkfs_volume *vol, uint64_t dir_oid,
                        const char *name, const char *target_path,
                        uint64_t *out_oid);
int embkfs_symlink_path(struct embkfs_volume *vol, uint64_t start_dir_oid,
                        const char *link_path, const char *target_path,
                        uint64_t *out_oid);
int embkfs_readlink_object(struct embkfs_volume *vol, uint64_t oid,
                           char *buf, size_t buf_sz, uint64_t *out_len);
int embkfs_readlink_path(struct embkfs_volume *vol, uint64_t start_dir_oid,
                         const char *path, char *buf, size_t buf_sz,
                         uint64_t *out_len);

/* Directory listing callback. `name` bytes are not null-terminated and valid
 * only for the duration of the callback. Return EMBK_OK to continue, or any
 * negative code to stop iteration and return that code. */
typedef int (*embkfs_dirent_cb)(uint64_t target_oid, uint8_t target_type,
                                const char *name, uint8_t name_len, void *ctx);

/* Iterate all directory entries for dir_oid in key order. */
int embkfs_list_dir(struct embkfs_volume *vol, uint64_t dir_oid,
                    embkfs_dirent_cb cb, void *ctx);
int embkfs_list_dir_page(struct embkfs_volume *vol, uint64_t dir_oid,
                         uint64_t start_index, uint64_t max_entries,
                         uint64_t *out_next_index, uint64_t *out_emitted,
                         embkfs_dirent_cb cb, void *ctx);
int embkfs_dir_entry_count(struct embkfs_volume *vol, uint64_t dir_oid,
                           uint64_t *out_count);
int embkfs_dir_is_empty(struct embkfs_volume *vol, uint64_t dir_oid,
                        bool *out_empty);
int embkfs_dir_exists_name(struct embkfs_volume *vol, uint64_t dir_oid,
                           const char *name, bool *out_exists,
                           uint8_t *out_type, uint64_t *out_oid);
int embkfs_dir_exists_path(struct embkfs_volume *vol, uint64_t start_dir_oid,
                           const char *path, bool *out_exists,
                           uint8_t *out_type, uint64_t *out_oid);

#endif /* _EMBKFS_H */