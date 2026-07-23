#ifndef _EMBKFS_H
#define _EMBKFS_H

#include "include/types.h"   /* bool, NULL, size_t */
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
#define EMBK_TYPE_SNAPSHOT   64   /* v2.2 Phase 5b: {ROOT_OBJECT_ID, SNAPSHOT, slot} */


/* ---- Mount-time constants (read-only, v2.2) ------------------------- */

/* The superblock checksum covers every byte BEFORE the trailing 8-byte
 * checksum field. Deriving it from the struct (total minus that final u64)
 * keeps it from ever drifting from the layout: 160 - 8 = 152. */
#define EMBKFS_SB_BODY_SIZE   (sizeof(struct embk_superblock) - sizeof(uint64_t))

/* Feature bits this reader understands. Any OTHER set incompat bit means
 * refuse to mount; any other set ro_compat bit means mount read-only.
 * COMPRESSION (v2.2 Phase 3): set on the superblock the first time a write
 * actually produces a COMPRESSED extent, so an older/non-aware reader
 * refuses the volume instead of silently serving compressed bytes as if
 * they were plaintext. ENCRYPTED (v2.2 Phase 4): set at format time,
 * requires the mount-time passphrase flow (see embk_crypto_header below)
 * before the volume's contents can be trusted at all. VERIFIED_ROOT
 * (v2.2 Phase 5d): set at format time, requires the mount-time HMAC check
 * (see embk_verify_header below) to pass before the volume is trusted --
 * INCOMPAT (not ro_compat/compat) so an older/non-checking reader can't
 * be used to bypass the check. */
#define EMBKFS_INCOMPAT_COMPRESSION   0x0000000000000001ULL
#define EMBKFS_INCOMPAT_ENCRYPTED     0x0000000000000002ULL
#define EMBKFS_INCOMPAT_VERIFIED_ROOT 0x0000000000000004ULL
/* SNAPREG (v2.3): the snapshot registry lives in a FIXED block outside the
 * CoW tree instead of as items inside it (see the snapshot section below).
 * INCOMPAT, not ro_compat, for a blunt reason: a reader that does not know
 * about the bit would see that block as free and hand it to the allocator,
 * overwriting the registry. Refusing the mount is the only safe answer. A
 * volume WITHOUT this bit still mounts and still works -- it uses the legacy
 * in-tree registry, with the rollback limitation that implies. */
#define EMBKFS_INCOMPAT_SNAPREG       0x0000000000000008ULL
#define EMBKFS_KNOWN_INCOMPAT   (EMBKFS_INCOMPAT_COMPRESSION | EMBKFS_INCOMPAT_ENCRYPTED | \
                                 EMBKFS_INCOMPAT_VERIFIED_ROOT | EMBKFS_INCOMPAT_SNAPREG)
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
    uint8_t  reserved[48]; /* 80  reserved[0..3] = writer_pid (v2.2 Phase 5c),
                             *     rest must be 0                          */
} __attribute__((packed));
_Static_assert(sizeof(struct embk_inode_item) == 128, "inode must be 128 bytes");

/* Process-provenance (v2.2 Phase 5c): which process's write last touched
 * this object's content, sourced from current_process->pid at every
 * content-mutating call (embkfs_make_object at creation,
 * embkfs_write_file at every write -- the same "did the DATA change"
 * distinction those two already use for btime vs mtime). 0 means
 * "unknown" (e.g. an inode from an image written by the mkfs oracle,
 * which has no process context to record). */
static inline uint32_t embk_inode_writer_pid(const struct embk_inode_item *ino) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v |= ((uint32_t)ino->reserved[i]) << (8 * i);
    return v;
}
static inline void embk_inode_set_writer_pid(struct embk_inode_item *ino, uint32_t pid) {
    for (int i = 0; i < 4; i++) ino->reserved[i] = (uint8_t)(pid >> (8 * i));
}

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

#define EMBKFS_EXTENT_F_HOLE       0x00000001u
#define EMBKFS_EXTENT_F_COMPRESSED 0x00000002u
#define EMBKFS_EXTENT_F_ENCRYPTED  0x00000004u

/* When EMBKFS_EXTENT_F_COMPRESSED is set, reserved1[0..7] holds the actual
 * compressed payload length (bytes) within the block-rounded run -- the
 * exact reserved-byte use the spec's own "future extent attributes
 * (compression metadata)" comment anticipated (v2.2 Phase 3). logical_size
 * keeps its existing meaning (the true, decompressed byte count); length
 * (in blocks) shrinks to whatever the compressed payload actually needs. */
static inline uint64_t embk_extent_compressed_size(const struct embk_extent_item *e) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= ((uint64_t)e->reserved1[i]) << (8 * i);
    return v;
}
static inline void embk_extent_set_compressed_size(struct embk_extent_item *e, uint64_t v) {
    for (int i = 0; i < 8; i++) e->reserved1[i] = (uint8_t)(v >> (8 * i));
}

/* ---- Encryption (v2.2 Phase 4) --------------------------------------
 * Gated by EMBKFS_INCOMPAT_ENCRYPTED. The crypto header lives at a fixed
 * offset within the SAME 512-byte sector as the superblock struct itself
 * (both superblock copies already get read as one sector each), but past
 * EMBKFS_SB_BODY_SIZE -- so it is NOT covered by the superblock's own
 * checksum. That's a deliberate scope reduction, not an oversight:
 * corruption here fails CLOSED (mount refused, or a legitimate passphrase
 * gets rejected) rather than open, so it costs availability, never
 * confidentiality/integrity of the encrypted data itself. Documented in
 * docs/EMBKFS_spec_v2.2.md alongside the other Phase 4 design decisions
 * (deterministic per-block-number XTS tweak, no ciphertext stealing). */
#define EMBKFS_CRYPTO_HEADER_OFFSET  200
#define EMBKFS_CRYPTO_HEADER_MAGIC   0x315952434B424D45ULL /* "EMBKCRY1" */

struct embk_crypto_header {
    uint64_t magic;                    /*  0  EMBKFS_CRYPTO_HEADER_MAGIC   */
    uint8_t  kdf_salt[16];             /*  8  PBKDF2 salt                  */
    uint32_t kdf_iterations;           /* 24  PBKDF2 iteration count       */
    uint8_t  key_check_ciphertext[16]; /* 28  XTS-encrypted known constant */
    uint8_t  reserved[8];              /* 44  must be 0                    */
} __attribute__((packed));
_Static_assert(sizeof(struct embk_crypto_header) == 52, "crypto header must be 52 bytes");

/* ---- Verified-root boot check (v2.2 Phase 5d) -----------------------
 * Gated by EMBKFS_INCOMPAT_VERIFIED_ROOT -- deliberately an INCOMPAT bit,
 * not compat/ro_compat: the entire point is that an implementation which
 * doesn't enforce the check must REFUSE the volume, not silently skip
 * verification (a ro_compat/compat bit would let a bypass be as simple as
 * mounting with an older reader).
 *
 * HONEST SCOPE (flagged, not silently reduced): this is HMAC-SHA256
 * authentication with a KEY EMBEDDED IN THE KERNEL BINARY
 * (EMBKFS_VERIFY_ROOT_KEY, embkfs.c), not real asymmetric signing. Anyone
 * with a copy of this kernel's source/binary can compute a valid HMAC for
 * any root they like -- this does NOT defend against an attacker who has
 * the kernel image, only against OFFLINE tampering by someone who
 * doesn't (a raw-disk edit, a swapped drive, a different/unmodified
 * kernel's write path). A true asymmetric upgrade (Ed25519 or similar) is
 * a natural v2.x follow-up, itself another crypto-primitive-sized body of
 * work on top of Phase 2's SHA-256/AES -- documented here and in
 * docs/EMBKFS_spec_v2.2.md rather than silently passed off as more than
 * it is.
 *
 * The HMAC is recomputed and re-stored by embkfs_write_superblock() on
 * EVERY commit (same atomic write as generation/root/free_blocks), over
 * exactly the bytes that matter -- the new root block_ptr (32 bytes) and
 * the new generation (8 bytes) -- so it stays valid across ordinary
 * writes made BY THIS KERNEL, and only ever mismatches when something
 * else touched the volume. */
#define EMBKFS_VERIFY_HEADER_OFFSET 260
#define EMBKFS_VERIFY_HEADER_MAGIC  0x315245564B424D45ULL /* "EMBKVER1" */

struct embk_verify_header {
    uint64_t magic;      /*  0  EMBKFS_VERIFY_HEADER_MAGIC                 */
    uint8_t  hmac[32];   /*  8  HMAC-SHA256(key, root(32) || generation(8)) */
    uint8_t  reserved[8]; /* 40  must be 0                                 */
} __attribute__((packed));
_Static_assert(sizeof(struct embk_verify_header) == 48, "verify header must be 48 bytes");

/* ---- Snapshots (v2.2 Phase 5b) --------------------------------------
 * A snapshot is nothing but a FROZEN root block_ptr: taking one is O(1)
 * (the tree it points at is already immutable, CoW guarantees no future
 * write ever mutates it in place). Stored as ordinary tree items --
 * {EMBKFS_ROOT_OBJECT_ID, EMBK_TYPE_SNAPSHOT, slot} -- reusing the exact
 * same transactional put/commit machinery as everything else, rather than
 * inventing a separate registry format.
 *
 * THE ROLLBACK LIMITATION, and its fix (v2.3, EMBKFS_INCOMPAT_SNAPREG).
 *
 * Storing the registry as ordinary tree items is elegant -- it reuses the
 * put/commit machinery instead of inventing a second format -- but it puts
 * the list of versions INSIDE the thing being versioned. Rolling back to an
 * older snapshot therefore reverts the registry too, and every snapshot taken
 * AFTER the rollback target stops existing in the restored tree (the way
 * `git reset --hard` drops refs that lived only in now-abandoned history).
 * Rollback was a one-way door: you could go back, but not back again.
 *
 * v2.3 moves the registry OUT of the tree, into one fixed block
 * (EMBKFS_SNAPREG_BLOCK) that no transaction ever rewrites. Rollback then
 * swaps the root and simply does not touch the registry, so snapshots on both
 * sides of the target survive and rollback becomes navigable in both
 * directions. The block is fixed rather than allocated for the same reason the
 * superblock's location is fixed: a pointer to it would itself have to live
 * somewhere durable, and a registry you must already have the registry to find
 * is no better off.
 *
 * The legacy in-tree layout is still read and written when the feature bit is
 * absent, so older volumes keep working exactly as before -- with the
 * limitation above intact, because on those volumes it is still true. */
#define EMBKFS_MAX_SNAPSHOTS    16
#define EMBKFS_SNAPSHOT_NAME_MAX 31   /* +1 reserved pad byte = 32 total */

/* Block 1: inside the pre-superblock region (blocks 1..15), which the
 * formatter has always left unused. Reserving one of those costs nothing and
 * needs no allocator support -- embkfs_bitmap_build() marks it alongside the
 * superblock and the backup, in the same "fixed metadata outside the tree"
 * step. Block 0 stays the null-pointer sentinel. */
#define EMBKFS_SNAPREG_BLOCK    1ULL
#define EMBKFS_SNAPREG_MAGIC    0x3147455250414E53ULL /* "SNAPREG1" */

/* On-disk registry block. checksum covers everything after it, exactly the
 * node-header convention (§8.1), so a torn or corrupted registry is DETECTED
 * rather than served as a list of plausible-looking roots -- which would be
 * worse than losing it, since a bad root_ptr sends the allocator marking
 * garbage as in-use. */
struct embk_snapshot_registry {
    uint64_t checksum;     /*  0  CRC32C over [8 .. sizeof(registry)-1] */
    uint64_t magic;        /*  8  EMBKFS_SNAPREG_MAGIC                  */
    uint32_t count;        /* 16  live entries in slots[0..count-1]     */
    uint32_t reserved;     /* 20  must be 0                             */
    /* slots follow: EMBKFS_MAX_SNAPSHOTS * sizeof(struct embk_snapshot_item) */
};

struct embk_snapshot_item {
    uint8_t  name[32];       /*  0  NUL-padded; NOT guaranteed NUL-terminated
                               *     if the name uses the full 31 usable bytes */
    struct embk_block_ptr root; /* 32  the frozen root pointer                */
    uint64_t generation;     /* 64  live generation at snapshot time          */
    uint64_t timestamp;      /* 72  rtc_now_ns() at snapshot time             */
} __attribute__((packed));
_Static_assert(sizeof(struct embk_snapshot_item) == 80, "snapshot item must be 80 bytes");


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
    uint64_t feature_incompat;       /* mirrors the on-disk field; only ever
                                       * gains bits (e.g. COMPRESSION the
                                       * first time a write actually
                                       * compresses an extent) -- v2.2 */
    bool     encrypted;               /* true if unlocked via a correct passphrase
                                       * at mount (v2.2 Phase 4) */
    /* Opaque storage for a `struct aes_xts_ctx` (kernel/crypto/xts.h) --
     * kept as raw bytes here rather than including xts.h so this widely-
     * included header stays free of a crypto-library type dependency.
     * embkfs.c static_asserts this is big enough and casts it. Valid only
     * when `encrypted` is true. */
    uint8_t  xts_opaque[512] __attribute__((aligned(16)));

    uint32_t snapshot_count;         /* cached count of EMBK_TYPE_SNAPSHOT
                                      * items, refreshed at mount and kept
                                      * in sync on create/delete -- >0 makes
                                      * the allocator hold freed blocks
                                      * instead of reclaiming them (v2.2
                                      * Phase 5b; see the snapshot section
                                      * above for why this is conservative
                                      * rather than exact refcounting) */

    /* Single-object read cache: the whole decoded contents of the most-recently
     * read object. embkfs_read_object_at used to re-decode the entire
     * [0, offset+want) prefix on EVERY call, making a sequentially-chunked read
     * of one file O(n^2); this serves later reads of the same file from RAM.
     * Keyed by (oid, generation) -- every write commit bumps `generation`, so a
     * stale entry is detected and refreshed automatically, no manual
     * invalidation needed. */
    uint64_t  rcache_oid;
    uint64_t  rcache_gen;
    uint64_t  rcache_len;
    uint8_t  *rcache_buf;

    /* Single-object EXTENT-MAP cache: the collected+validated extref array of
     * the most-recently ranged-over object. Same (oid, generation) keying as
     * rcache above, and needed for the same reason one layer down.
     *
     * rcache only covers objects <= EMBKFS_RCACHE_MAX; a BIGGER file falls back
     * to embkfs_read_object_range(), which had to count + collect + validate the
     * whole extent map on EVERY read (two B-tree scans + a kmalloc each time).
     * With thousands of extents that dominated, so skipping the prefix decode
     * alone bought nothing measurable.
     *
     * This is deliberately the EXTENT MAP, not the data: ~32 B per extent, so a
     * 10 MB file costs ~100 KB here versus 10 MB in rcache -- which is exactly
     * why raising RCACHE_MAX is the wrong lever. */
    uint64_t  ecache_oid;
    uint64_t  ecache_gen;
    uint32_t  ecache_n;
    struct embk_extref *ecache_ext;

    /* One byte per extent: "this extent's checksum has already been verified in
     * this generation". THE point of it: mkfs writes ONE EXTENT PER FILE, and an
     * extent's checksum covers the WHOLE extent -- so a partial read had to read
     * and CRC every block of a 10 MB extent just to hand back 8 KB, making reads
     * O(filesize) EACH. First read still verifies in full (integrity unchanged);
     * later reads in the same generation trust that and touch only the blocks
     * they actually need. Freed/rebuilt with ecache_ext, keyed the same way, so a
     * write commit's generation bump forces re-verification. */
    uint8_t  *ecache_verified;

    /* Single-object INODE cache. Third and last of the (oid, generation)-keyed
     * caches, and the one that mattered most per-read: locating an inode is a
     * B-tree descent that READS BLOCKS, and every read did TWO of them --
     * read_object_at's size probe plus read_object_range's own lookup. Measured:
     * ~3 device reads per 8 KB chunk when only ONE is the actual data block.
     * A single 128-byte struct, so it costs nothing next to rcache's megabytes. */
    uint64_t  icache_oid;
    uint64_t  icache_gen;
    bool      icache_valid;
    struct embk_inode_item icache_ino;

    /* Windowed read-ahead cache. rcache is all-or-nothing: it caches the WHOLE
     * object, so `total <= EMBKFS_RCACHE_MAX` is a cliff -- a file one byte over
     * the cap gets NO caching at all and every read goes to the device.
     * python314.zip (10.3 MB vs an 8 MB cap) lands just past it, which is why
     * CPython's startup reads were served entirely from disk.
     *
     * This caches a fixed-size WINDOW instead, so cost is bounded for any file
     * size -- including the multi-GB case the whole-object cache could never
     * serve. It is also the only way past the measured floor: one device request
     * costs ~2.7ms regardless of size, so an 8 KB read can never beat
     * 8KB/2.7ms; reading a window ahead amortises one request over many reads.
     * Keyed (oid, generation) like the others, plus the window offset. */
    uint8_t  *wcache_buf;   /* EMBKFS_WCACHE_WIN bytes, or NULL when unallocated */
    uint64_t  wcache_oid;
    uint64_t  wcache_gen;
    uint64_t  wcache_off;   /* window-aligned file offset of wcache_buf[0] */
    uint64_t  wcache_len;   /* valid bytes; < WIN only when the window hits EOF */
};

/* Where EMBKFS's own block reads go, split by cause. Capping extent size made the
 * isolated read path better (443% -> 101% amplification) but raised the WHOLE
 * test's device reads above where they started -- more extents means more B-tree
 * items, and both embkfs_count_extents() and embkfs_collect_extents() walk every
 * leaf holding an object's extents, reading a node per visit. That is a STORY,
 * and stories have been wrong six times in this rebuild; these counters decide
 * it. See `test posix` / `test ioperf`. */
struct embkfs_stat {
    uint64_t node_reads;     /* embkfs_read_node calls = B-tree blocks read */
    uint64_t prefix_calls;   /* read_object_prefix: collects extents EVERY call */
    uint64_t ecache_hit;
    uint64_t ecache_miss;    /* each miss = count_extents + collect_extents */
};
void embkfs_stat_reset(void);
void embkfs_stat_get(struct embkfs_stat *out);


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
/* v2.2 (Phase 1): how many EMBKFS volumes embkfs_init() will mount
 * simultaneously -- previously the mount-probe loop stopped at the FIRST
 * block device with a valid superblock; every other one, including a
 * USB-attached volume sharing the machine with an internal disk, was
 * silently ignored. Small and fixed like MAX_PROCESSES/MAX_CPUS elsewhere
 * in this kernel -- revisit only if this is ever actually hit in practice. */
#define EMBKFS_MAX_VOLUMES 4

void embkfs_init(void);
/* The PRIMARY volume: the first one embkfs_init() found and mounted,
 * always registered at "/" by main.c. Every existing internal caller
 * (the selftests, in particular) operates on this one implicitly and is
 * completely unaffected by additional volumes existing alongside it --
 * see embkfs_volume_count()/embkfs_volume_at() below for those. */
struct embkfs_volume *embkfs_live_volume(void);

/* How many volumes are currently mounted (0..EMBKFS_MAX_VOLUMES), and a
 * mounted volume by index in mount order (index 0 == embkfs_live_volume()).
 * main.c uses these to register every volume BEYOND the primary at its own
 * VFS mount point; NULL for an out-of-range index. */
uint32_t embkfs_volume_count(void);
struct embkfs_volume *embkfs_volume_at(uint32_t index);
int embkfs_vfs_register(const char *path, struct embkfs_volume *vol);

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
int embkfs_run_shrink_selftests(void);
int embkfs_run_timestamp_selftests(void);
int embkfs_run_multivol_selftests(void);
int embkfs_run_compress_selftests(void);
int embkfs_run_selfheal_selftests(void);
int embkfs_run_snapshot_selftests(void);
int embkfs_run_snapreg_selftests(void);
int embkfs_run_provenance_selftests(void);
int embkfs_run_verifyboot_selftests(void);
int embkfs_run_namespace_selftests(void);
int embkfs_run_boot_diagnostics(void);

/* Snapshots (v2.2 Phase 5b). `out_items`/`max` may be NULL/0 to just get
 * *out_n. `name` is truncated to EMBKFS_SNAPSHOT_NAME_MAX bytes. */
int embkfs_snapshot_create(struct embkfs_volume *vol, const char *name);
int embkfs_snapshot_delete(struct embkfs_volume *vol, const char *name);
int embkfs_snapshot_rollback(struct embkfs_volume *vol, const char *name);
int embkfs_snapshot_list(struct embkfs_volume *vol, struct embk_snapshot_item *out_items,
                         uint32_t max, uint32_t *out_n);

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
/* `stat`-style metadata lookup: copies oid's raw inode item (size, mode,
 * links, uid/gid, flags, atime/mtime/ctime/btime, generation) out to *out. */
int embkfs_stat_object(struct embkfs_volume *vol, uint64_t oid,
                       struct embk_inode_item *out);
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
/* Set an object's PERMISSION bits (file-type bits are preserved from the
 * existing inode; ctime moves, mtime does not). Real: inodes have always had a
 * mode, there was just no road here from userspace. */
int embkfs_chmod_object(struct embkfs_volume *vol, uint64_t oid, uint32_t mode);

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

/* Open-handle refcounting for unlink-while-open safety. The fd layer brackets
 * each open file with get...put; the object is reclaimed only when its on-disk
 * link count and its open count are both zero. */
int embkfs_object_get(struct embkfs_volume *vol, uint64_t oid);
int embkfs_object_put(struct embkfs_volume *vol, uint64_t oid);

#endif /* _EMBKFS_H */