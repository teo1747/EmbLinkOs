"""
layout.py — EMBKFS on-disk structures, in Python, matching Specification v2.0
exactly. Every struct here uses little-endian packing (the spec mandates
little-endian on disk regardless of CPU). Sizes are asserted so a mistake is
caught immediately rather than producing a subtly-wrong image.

This module is the concrete embodiment of the spec's byte layouts. Reading it
alongside the spec is the clearest way to see how the format is laid down.
"""

import struct

# ---------------------------------------------------------------------------
# Geometry
# ---------------------------------------------------------------------------
BLOCK_SIZE      = 4096          # default block size (spec §3)
SECTOR_SIZE     = 512           # underlying device sector
SECTORS_PER_BLK = BLOCK_SIZE // SECTOR_SIZE  # = 8

SUPERBLOCK_OFFSET = 65536       # primary superblock fixed byte offset (spec §4.1)

# ---------------------------------------------------------------------------
# Magic numbers (spec §5, §8.1)
# ---------------------------------------------------------------------------
# Stored little-endian, these read "EMBKFS17" and "EMBKNODE" in a hex dump.
EMBKFS_MAGIC      = 0x373153464B424D45   # "EMBKFS17"
EMBKFS_NODE_MAGIC = 0x45444F4E4B424D45   # "EMBKNODE"

# ---------------------------------------------------------------------------
# Version (spec §5.1)
# ---------------------------------------------------------------------------
VERSION_MAJOR = 1
VERSION_MINOR = 0

# ---------------------------------------------------------------------------
# Item types — the key's `type` field (spec §9). Inode lowest so it sorts first.
# ---------------------------------------------------------------------------
EMBK_TYPE_INODE     = 1
EMBK_TYPE_DIR_ENTRY = 16
EMBK_TYPE_EXTENT    = 32
EMBK_TYPE_XATTR     = 48

# ---------------------------------------------------------------------------
# POSIX st_mode bits (spec §9.1)
# ---------------------------------------------------------------------------
S_IFMT  = 0o170000   # type mask
S_IFDIR = 0o040000   # directory
S_IFREG = 0o100000   # regular file
S_IFLNK = 0o120000   # symlink
# typical permission sets
PERM_DIR  = 0o755
PERM_FILE = 0o644
PERM_LNK  = 0o777

# target_type values stored in directory entries (spec §9.2) — small enum,
# mirrors the inode's object type so readdir need not fetch the inode.
DT_REG = 1   # regular file
DT_DIR = 2   # directory
DT_LNK = 3   # symlink

# Reserved object id for the root directory. Object ids 0 and 1 are reserved by
# convention (0 = "none/null", 1 = root directory), mirroring how FAT/ext keep
# low ids special. First user object starts at 2.
OBJID_NONE = 0
OBJID_ROOT = 1
FIRST_USER_OBJID = 2


# ===========================================================================
# Block pointer (spec §6) — 32 bytes: {block, checksum, generation, flags}
# ===========================================================================
BLOCK_PTR_FMT  = "<QQQQ"
BLOCK_PTR_SIZE = struct.calcsize(BLOCK_PTR_FMT)
assert BLOCK_PTR_SIZE == 32, BLOCK_PTR_SIZE

def pack_block_ptr(block, checksum, generation, flags=0):
    """Pack a 32-byte fat pointer. checksum is the CRC32C of the TARGET block."""
    return struct.pack(BLOCK_PTR_FMT, block, checksum, generation, flags)

def null_block_ptr():
    """An all-zero block pointer (points at nothing)."""
    return pack_block_ptr(0, 0, 0, 0)


# ===========================================================================
# Key (spec §7) — 24 bytes: {object_id, type, offset}
# ===========================================================================
KEY_FMT  = "<QQQ"
KEY_SIZE = struct.calcsize(KEY_FMT)
assert KEY_SIZE == 24, KEY_SIZE

def pack_key(object_id, type_, offset):
    return struct.pack(KEY_FMT, object_id, type_, offset)


# ===========================================================================
# Node header (spec §8.1) — 40 bytes
#   checksum(8) magic(8) generation(8) block(8) level(1) reserved(3) nritems(4)
# ===========================================================================
NODE_HDR_FMT  = "<QQQQB3sI"
NODE_HDR_SIZE = struct.calcsize(NODE_HDR_FMT)
assert NODE_HDR_SIZE == 40, NODE_HDR_SIZE

def pack_node_header(checksum, generation, block, level, nritems):
    return struct.pack(NODE_HDR_FMT,
                       checksum, EMBKFS_NODE_MAGIC, generation, block,
                       level, b"\x00\x00\x00", nritems)


# ===========================================================================
# Leaf item header (spec §8.3) — 32 bytes: key(24) offset(4) size(4)
#   offset = where the item's data is, measured from the START of the block
# ===========================================================================
ITEM_HDR_FMT  = "<24sII"   # 24-byte key blob, then offset, then size
ITEM_HDR_SIZE = struct.calcsize(ITEM_HDR_FMT)
assert ITEM_HDR_SIZE == 32, ITEM_HDR_SIZE

def pack_item_header(key_bytes, data_offset, data_size):
    assert len(key_bytes) == KEY_SIZE
    return struct.pack(ITEM_HDR_FMT, key_bytes, data_offset, data_size)


# ===========================================================================
# Inode item (spec §9.1) — 128 bytes
# ===========================================================================
#   size(8) blocks(8) links(8)          = 24
#   mode(4) uid(4) gid(4) flags(4)      = 16
#   atime(8) mtime(8) ctime(8) btime(8) = 32
#   generation(8)                       =  8   -> real fields total 80
#   reserved(48)                        = 48   -> padded to a clean 128
INODE_FMT  = "<QQQ IIII QQQQ Q 48s"
INODE_SIZE = struct.calcsize(INODE_FMT)
assert INODE_SIZE == 128, INODE_SIZE

def pack_inode(size, blocks, links, mode, uid, gid,
               atime=0, mtime=0, ctime=0, btime=0, generation=0, flags=0):
    return struct.pack(INODE_FMT,
                       size, blocks, links, mode, uid, gid, flags,
                       atime, mtime, ctime, btime, generation,
                       b"\x00" * 48)


# ===========================================================================
# Directory entry item (spec §9.2) — 16-byte fixed part + name bytes
#   target_object_id(8) target_type(1) name_len(1) reserved(6) + name
# ===========================================================================
DIR_ENTRY_FIXED_FMT  = "<QBB6s"
DIR_ENTRY_FIXED_SIZE = struct.calcsize(DIR_ENTRY_FIXED_FMT)
assert DIR_ENTRY_FIXED_SIZE == 16, DIR_ENTRY_FIXED_SIZE

def pack_dir_entry(target_object_id, target_type, name: bytes):
    assert 1 <= len(name) <= 255, "name length must be 1..255 (POSIX NAME_MAX)"
    fixed = struct.pack(DIR_ENTRY_FIXED_FMT,
                        target_object_id, target_type, len(name), b"\x00" * 6)
    return fixed + name   # name stored inline, not null-terminated


# ===========================================================================
# Extent item (spec §9.3) — 64 bytes
#   disk_block(8) length(8) logical_size(8) checksum(8) generation(8)
#   flags(4) reserved0(4) reserved1(16)
# ===========================================================================
EXTENT_FMT  = "<QQQQQ II 16s"
EXTENT_SIZE = struct.calcsize(EXTENT_FMT)
assert EXTENT_SIZE == 64, EXTENT_SIZE

# extent flag bits (spec §9.3)
EXTENT_FLAG_HOLE       = 1 << 0
EXTENT_FLAG_COMPRESSED = 1 << 1
EXTENT_FLAG_ENCRYPTED  = 1 << 2
# Kernel-header naming aliases.
EMBKFS_EXTENT_F_HOLE = EXTENT_FLAG_HOLE

def pack_extent(disk_block, length_blocks, logical_size, data_checksum,
                generation=0, flags=0):
    return struct.pack(EXTENT_FMT,
                       disk_block, length_blocks, logical_size,
                       data_checksum, generation, flags, 0, b"\x00" * 16)


# ===========================================================================
# Superblock (spec §5.2)
#   magic(8) version_major(4) version_minor(4)
#   feature_compat(8) feature_ro_compat(8) feature_incompat(8)
#   block_size(8) total_blocks(8) free_blocks(8) uuid(16) generation(8)
#   root(block_ptr, 32) checkpoint(block_ptr, 32) checksum(8)
# ===========================================================================
# Up to (but not including) the trailing checksum:
SB_BODY_FMT = "<Q II QQQ QQQ 16s Q 32s 32s"
SB_BODY_SIZE = struct.calcsize(SB_BODY_FMT)
# total = body + 8-byte checksum
SUPERBLOCK_SIZE = SB_BODY_SIZE + 8

def pack_superblock_body(block_size, total_blocks, free_blocks,
                         uuid16, generation, root_ptr32, checkpoint_ptr32,
                         feat_compat=0, feat_ro_compat=0, feat_incompat=0):
    assert len(uuid16) == 16
    assert len(root_ptr32) == 32
    assert len(checkpoint_ptr32) == 32
    return struct.pack(SB_BODY_FMT,
                       EMBKFS_MAGIC, VERSION_MAJOR, VERSION_MINOR,
                       feat_compat, feat_ro_compat, feat_incompat,
                       block_size, total_blocks, free_blocks,
                       uuid16, generation, root_ptr32, checkpoint_ptr32)


if __name__ == "__main__":
    # Print every structure size so we can eyeball them against the spec.
    print("EMBKFS structure sizes (must match spec v2.0):")
    print(f"  block pointer      : {BLOCK_PTR_SIZE:4d}  (spec: 32)")
    print(f"  key                : {KEY_SIZE:4d}  (spec: 24)")
    print(f"  node header        : {NODE_HDR_SIZE:4d}  (spec: 40)")
    print(f"  item header        : {ITEM_HDR_SIZE:4d}  (spec: 32)")
    print(f"  inode item         : {INODE_SIZE:4d}  (spec: 128)")
    print(f"  dir entry (fixed)  : {DIR_ENTRY_FIXED_SIZE:4d}  (spec: 16)")
    print(f"  extent item        : {EXTENT_SIZE:4d}  (spec: 64)")
    print(f"  superblock body    : {SB_BODY_SIZE:4d}")
    print(f"  superblock total   : {SUPERBLOCK_SIZE:4d}")
    print(f"  internal slot      : {KEY_SIZE + BLOCK_PTR_SIZE:4d}  (spec: 56)")
    print(f"  block body bytes   : {BLOCK_SIZE - NODE_HDR_SIZE:4d}  (spec: 4056)")
    print("All struct sizes asserted OK at import.")
