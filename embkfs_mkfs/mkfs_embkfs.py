#!/usr/bin/env python3
"""
mkfs_embkfs.py — a minimal EMBKFS formatter (the "mkfs" for EMBKFS).

It writes a known-good EMBKFS image that the EmbLink kernel's read-only mount
path can validate against — the same role mkfs.vfat played for FAT. The image is
deliberately small but exercises the WHOLE format:

    - a primary superblock at byte 65536 (and a backup at the last block)
    - a single metadata LEAF node (level 0) that is also the tree root
    - inside that leaf, as key-sorted items:
        * the root directory's inode             {OBJID_ROOT, INODE, 0}
        * the root directory's entries           {OBJID_ROOT, DIR_ENTRY, hash}
          (one item per name hash; names that COLLIDE on the hash share one item
           as a chain of records — see build_dir_entry_items)
        * each file's inode                      {fileid, INODE, 0}
        * each file's single extent              {fileid, EXTENT, 0}
    - one data block per file

The default image holds regular files plus one symlink in the root directory:
    hello.txt    — an ordinary entry (its own single-record dir-entry item)
    wgyehkb.txt  } these two share the SAME CRC32C name hash (0xC38842AB), so
    illoeuw.txt  } they land in ONE dir-entry item as a collision chain.
    hello.lnk    — a symlink object whose payload stores "hello.txt"

Everything fits in a single leaf, so we need no internal nodes yet — the
smallest image that still tests the superblock, node header, leaf layout,
field-wise key ordering, collision chaining, and every item type.

Block map (4 KiB blocks):
    block 16     : primary superblock (byte offset 65536)
    block 17     : the metadata leaf (tree root)
    block 18..   : one data block per file
    last block   : backup superblock
"""

import sys
import struct
import uuid as uuidlib

from crc32c import crc32c
import layout as L


# --- helper: CRC32C-based name hash for directory-entry keys (spec §7.2/§9.2) ---
def name_hash(name: bytes) -> int:
    """
    The directory-entry key's `offset` field is a hash of the name. The spec says
    CRC32C-based; we define it concretely as the 32-bit CRC32C of the name bytes,
    placed in the low 32 bits of the 64-bit offset. The kernel must use the
    identical construction to find entries by name.
    """
    return crc32c(name) & 0xFFFFFFFF


def build_dir_entry_items(dir_oid: int, entries: list) -> list:
    """
    Build the DIR_ENTRY items for one directory.

    `entries` is a list of (name_bytes, target_oid, target_type). Entries are
    grouped by name hash: names that share a hash become ONE item whose payload
    is their records concatenated — a collision chain (spec §9.2). The key stays
    unique (one item per hash value); collisions live inside the payload. With no
    collisions, every group has a single entry and the output is identical to
    plain single-record dir-entry items.

    Each record is self-describing (its name_len gives its length) and the item's
    `size` bounds the chain, so no explicit entry count is stored.
    """
    groups = {}                                  # hash -> [(name, oid, typ), ...]
    for name, oid, typ in entries:
        groups.setdefault(name_hash(name), []).append((name, oid, typ))

    items = []
    for h, group in groups.items():
        group.sort(key=lambda g: g[0])           # deterministic order within the chain
        payload = b"".join(L.pack_dir_entry(oid, typ, name)
                           for (name, oid, typ) in group)
        items.append((L.pack_key(dir_oid, L.EMBK_TYPE_DIR_ENTRY, h), payload))
    return items


def build_leaf_block(generation: int, block_no: int, items: list) -> bytes:
    """
    Build one 4 KiB leaf node from a list of (key_bytes, data_bytes), already
    sorted by key. Implements the spec §8.3 slotted layout:

        [ node_header(40) ][ item_header[0..n) growing down ]
        ...... free space ......
        [ item_data growing UP from the end of the block ]

    The checksum (first 8 bytes of the header) is computed last, over bytes
    [8 .. block_size-1], matching the kernel.
    """
    bs = L.BLOCK_SIZE
    buf = bytearray(bs)

    n = len(items)

    # Item data is placed at the TAIL; data_cursor walks downward from the end.
    data_cursor = bs
    data_offsets = [0] * n
    for i in range(n):
        _key, data = items[i]
        data_cursor -= len(data)
        data_offsets[i] = data_cursor
        buf[data_cursor:data_cursor + len(data)] = data

    # Headers occupy [40, 40 + n*32); they must not collide with the data region.
    headers_end = L.NODE_HDR_SIZE + n * L.ITEM_HDR_SIZE
    if headers_end > data_cursor:
        raise ValueError("leaf overflow: items do not fit in one block")

    # Write the item headers (front array), in sorted key order.
    pos = L.NODE_HDR_SIZE
    for i in range(n):
        key, data = items[i]
        hdr = L.pack_item_header(key, data_offsets[i], len(data))
        buf[pos:pos + L.ITEM_HDR_SIZE] = hdr
        pos += L.ITEM_HDR_SIZE

    # Node header with checksum=0 for now (level 0 = leaf).
    header = L.pack_node_header(checksum=0, generation=generation,
                                block=block_no, level=0, nritems=n)
    buf[0:L.NODE_HDR_SIZE] = header

    # Checksum over bytes [8 .. end], patched into the first 8 bytes.
    csum = crc32c(bytes(buf[8:]))
    struct.pack_into("<Q", buf, 0, csum)

    return bytes(buf)


def build_data_block(data: bytes) -> bytes:
    """A file data block: the bytes, zero-padded to a full block."""
    if len(data) > L.BLOCK_SIZE:
        raise ValueError("this minimal formatter supports a single-block file")
    buf = bytearray(L.BLOCK_SIZE)
    buf[0:len(data)] = data
    return bytes(buf)


def build_superblock(total_blocks: int, free_blocks: int, generation: int,
                     uuid16: bytes, root_block: int, root_csum: int) -> bytes:
    """
    Build the superblock block (4 KiB): body per spec §5.2 plus the trailing
    checksum over all preceding bytes. The rest of the block is reserved (zero).
    """
    root_ptr = L.pack_block_ptr(block=root_block, checksum=root_csum,
                                generation=generation, flags=0)
    checkpoint_ptr = L.null_block_ptr()   # no checkpoint in this minimal image

    body = L.pack_superblock_body(
        block_size=L.BLOCK_SIZE,
        total_blocks=total_blocks,
        free_blocks=free_blocks,
        uuid16=uuid16,
        generation=generation,
        root_ptr32=root_ptr,
        checkpoint_ptr32=checkpoint_ptr,
    )

    sb_csum = crc32c(body)
    sb = body + struct.pack("<Q", sb_csum)
    assert len(sb) == L.SUPERBLOCK_SIZE

    block = bytearray(L.BLOCK_SIZE)
    block[0:len(sb)] = sb
    return bytes(block)


def make_image(path: str, size_bytes: int = 1024 * 1024):
    bs = L.BLOCK_SIZE
    total_blocks = size_bytes // bs
    gen = 1

    # --- fixed block assignments ---
    SB_BLOCK     = L.SUPERBLOCK_OFFSET // bs    # 65536/4096 = block 16
    LEAF_BLOCK   = SB_BLOCK + 1                 # block 17: metadata leaf (root)
    DATA_BLOCK   = SB_BLOCK + 2                 # block 18: first file's data
    BACKUP_BLOCK = total_blocks - 1            # last block: backup superblock

    # --- objects we put in the root directory ---
    # hello.txt is an ordinary single-entry item (the regression case).
    # wgyehkb.txt and illoeuw.txt share the SAME CRC32C name hash (0xC38842AB),
    # so they land in ONE dir-entry item as a 2-record collision chain.
    objects = [
        (b"hello.txt",   L.DT_REG, L.S_IFREG | L.PERM_FILE,
         b"Hello from EMBKFS! This file was written by mkfs_embkfs.py.\n"),
        (b"wgyehkb.txt", L.DT_REG, L.S_IFREG | L.PERM_FILE,
         b"Colliding file A: wgyehkb.txt resolves to object 3.\n"),
        (b"illoeuw.txt", L.DT_REG, L.S_IFREG | L.PERM_FILE,
         b"Colliding file B: illoeuw.txt resolves to object 4.\n"),
        (b"hello.lnk",   L.DT_LNK, L.S_IFLNK | L.PERM_LNK,
         b"hello.txt"),
    ]

    # --- build the leaf items (sorted by key at the end) ---
    items = []

    # root directory inode: {OBJID_ROOT, INODE, 0}
    items.append((L.pack_key(L.OBJID_ROOT, L.EMBK_TYPE_INODE, 0),
                  L.pack_inode(size=0, blocks=0, links=2,
                               mode=L.S_IFDIR | L.PERM_DIR, uid=0, gid=0,
                               generation=gen)))

    # one object id + data block + inode + extent per file; collect the dir
    # entries so they can be grouped/chained by name hash afterwards.
    dir_entries = []
    data_blocks = []                              # (block_no, block_bytes) to write
    oid = L.FIRST_USER_OBJID                      # 2
    blk = DATA_BLOCK                              # 18
    for name, dtype, mode, data in objects:
        dir_entries.append((name, oid, dtype))

        items.append((L.pack_key(oid, L.EMBK_TYPE_INODE, 0),
                      L.pack_inode(size=len(data), blocks=1, links=1,
                                   mode=mode, uid=0, gid=0,
                                   generation=gen)))

        # data checksum is CRC32C over the file's actual bytes (logical_size),
        # NOT the zero padding — spec §9.3 end-to-end data integrity.
        items.append((L.pack_key(oid, L.EMBK_TYPE_EXTENT, 0),
                      L.pack_extent(disk_block=blk, length_blocks=1,
                                    logical_size=len(data),
                                    data_checksum=crc32c(data), generation=gen)))

        data_blocks.append((blk, build_data_block(data)))
        oid += 1
        blk += 1

    # the directory's entries, grouped + chained by name hash
    items.extend(build_dir_entry_items(L.OBJID_ROOT, dir_entries))

    # Sort by FIELD-WISE key order (object_id, type, offset) as INTEGERS — not by
    # raw little-endian key bytes. Tuple comparison matches the kernel's
    # embk_key_cmp; a memcmp of LE bytes orders large offset hashes wrong (e.g.
    # hello.txt's 0x4A9EC2EE vs the collision chain's 0xC38842AB).
    items.sort(key=lambda it: struct.unpack(L.KEY_FMT, it[0]))

    # --- build the metadata leaf ---
    leaf = build_leaf_block(generation=gen, block_no=LEAF_BLOCK, items=items)
    leaf_csum = crc32c(leaf[8:])  # the leaf's checksum is over bytes [8..end]
    embedded = struct.unpack_from("<Q", leaf, 0)[0]
    assert embedded == leaf_csum, "leaf self-checksum mismatch in builder"

    # free_blocks hint: block 0 (reserved as the null-pointer sentinel) + SB +
    # leaf + backup (4) plus one data block per file. Block 0 is never written
    # but is counted used so the kernel's allocator oracle (which reserves it)
    # agrees with this hint.
    used = 4 + len(objects)
    free_blocks = total_blocks - used

    superblock = build_superblock(total_blocks=total_blocks,
                                  free_blocks=free_blocks, generation=gen,
                                  uuid16=uuidlib.uuid4().bytes,
                                  root_block=LEAF_BLOCK, root_csum=leaf_csum)

    # --- assemble the full image ---
    img = bytearray(size_bytes)  # all zeros

    def put(block_no, block_bytes):
        off = block_no * bs
        img[off:off + len(block_bytes)] = block_bytes

    put(SB_BLOCK, superblock)
    put(LEAF_BLOCK, leaf)
    for b, d in data_blocks:
        put(b, d)
    put(BACKUP_BLOCK, superblock)   # backup is an identical copy

    with open(path, "wb") as f:
        f.write(img)

    # --- report ---
    print(f"Wrote {path}  ({size_bytes} bytes, {total_blocks} blocks of {bs})")
    print(f"  superblock      : block {SB_BLOCK} (byte {SB_BLOCK*bs})")
    print(f"  metadata leaf   : block {LEAF_BLOCK}  (checksum 0x{leaf_csum:08X}, {len(items)} items)")
    for (b, _d), (name, _dtype, _mode, data) in zip(data_blocks, objects):
        print(f"  file data       : block {b}  (\"{name.decode()}\", {len(data)} bytes,"
              f" data csum 0x{crc32c(data):08X})")
    print(f"  backup superblk : block {BACKUP_BLOCK} (last block)")
    print(f"  free_blocks hint: {free_blocks}")
    print()
    print("  Items in the leaf (key-sorted):")
    for key, data in items:
        oid_, typ, off = struct.unpack(L.KEY_FMT, key)
        tname = {L.EMBK_TYPE_INODE: "INODE", L.EMBK_TYPE_DIR_ENTRY: "DIR_ENTRY",
                 L.EMBK_TYPE_EXTENT: "EXTENT", L.EMBK_TYPE_XATTR: "XATTR"}.get(typ, str(typ))
        print(f"    {{obj={oid_}, type={tname:<9}, off=0x{off:08X}}}  ({len(data)} bytes data)")


SLOT = L.KEY_SIZE + L.BLOCK_PTR_SIZE   # 24 + 32 = 56


def build_internal_block(generation, block_no, level, slots):
    """Build one internal node (level > 0). Unlike a leaf, an internal node is
    the node_header(40) followed by a CONTIGUOUS array of {key(24), block_ptr(32)}
    = 56-byte slots, sorted by key — no slotted-page indirection, since slots are
    fixed-size. Each slot's key is the SMALLEST key in that child's subtree.
    nritems = number of slots; checksum over [8..end] like any node."""
    bs = L.BLOCK_SIZE
    buf = bytearray(bs)
    n = len(slots)
    if L.NODE_HDR_SIZE + n * SLOT > bs:
        raise ValueError("too many slots for one internal node")
    buf[0:L.NODE_HDR_SIZE] = L.pack_node_header(checksum=0, generation=generation,
                                                block=block_no, level=level, nritems=n)
    pos = L.NODE_HDR_SIZE
    for key_bytes, ptr_bytes in slots:
        buf[pos:pos + L.KEY_SIZE] = key_bytes
        buf[pos + L.KEY_SIZE:pos + SLOT] = ptr_bytes
        pos += SLOT
    struct.pack_into("<Q", buf, 0, crc32c(bytes(buf[8:])))
    return bytes(buf)


def make_tree_image(path: str, size_bytes: int = 1024 * 1024):
    """Same files as make_image, but the leaf items are split across TWO leaves
    with an internal root node above them — a real 2-level B-tree that exercises
    the descent path. The split lands on object 3 so its inode key sits exactly
    on a slot key (the <= boundary case)."""
    bs = L.BLOCK_SIZE
    total_blocks = size_bytes // bs
    gen = 1

    SB_BLOCK    = L.SUPERBLOCK_OFFSET // bs   # 16
    ROOT_BLOCK  = SB_BLOCK + 1                # 17: internal root (level 1)
    LEAFA_BLOCK = SB_BLOCK + 2               # 18
    LEAFB_BLOCK = SB_BLOCK + 3               # 19
    DATA_START  = SB_BLOCK + 4               # 20..: file data
    BACKUP_BLOCK = total_blocks - 1

    objects = [
        (b"hello.txt",   L.DT_REG, L.S_IFREG | L.PERM_FILE,
         b"Hello from EMBKFS! This file was written by mkfs_embkfs.py.\n"),
        (b"wgyehkb.txt", L.DT_REG, L.S_IFREG | L.PERM_FILE,
         b"Colliding file A: wgyehkb.txt resolves to object 3.\n"),
        (b"illoeuw.txt", L.DT_REG, L.S_IFREG | L.PERM_FILE,
         b"Colliding file B: illoeuw.txt resolves to object 4.\n"),
        (b"hello.lnk",   L.DT_LNK, L.S_IFLNK | L.PERM_LNK,
         b"hello.txt"),
    ]

    items = []
    items.append((L.pack_key(L.OBJID_ROOT, L.EMBK_TYPE_INODE, 0),
                  L.pack_inode(size=0, blocks=0, links=2, mode=L.S_IFDIR | L.PERM_DIR,
                               uid=0, gid=0, generation=gen)))
    dir_entries, data_blocks = [], []
    oid, blk = L.FIRST_USER_OBJID, DATA_START
    for name, dtype, mode, data in objects:
        dir_entries.append((name, oid, dtype))
        items.append((L.pack_key(oid, L.EMBK_TYPE_INODE, 0),
                      L.pack_inode(size=len(data), blocks=1, links=1, mode=mode,
                                   uid=0, gid=0, generation=gen)))
        items.append((L.pack_key(oid, L.EMBK_TYPE_EXTENT, 0),
                      L.pack_extent(disk_block=blk, length_blocks=1, logical_size=len(data),
                                    data_checksum=crc32c(data), generation=gen)))
        data_blocks.append((blk, build_data_block(data)))
        oid += 1
        blk += 1
    items.extend(build_dir_entry_items(L.OBJID_ROOT, dir_entries))
    items.sort(key=lambda it: struct.unpack(L.KEY_FMT, it[0]))

    # split the items across two leaves; the internal root routes between them
    split = 5
    leafA_items, leafB_items = items[:split], items[split:]
    leafA = build_leaf_block(gen, LEAFA_BLOCK, leafA_items)
    leafB = build_leaf_block(gen, LEAFB_BLOCK, leafB_items)
    leafA_csum, leafB_csum = crc32c(leafA[8:]), crc32c(leafB[8:])

    # slot key = smallest key in that child's subtree = its first item's key
    slots = [
        (leafA_items[0][0], L.pack_block_ptr(LEAFA_BLOCK, leafA_csum, gen)),
        (leafB_items[0][0], L.pack_block_ptr(LEAFB_BLOCK, leafB_csum, gen)),
    ]
    root = build_internal_block(gen, ROOT_BLOCK, level=1, slots=slots)
    root_csum = crc32c(root[8:])

    # block 0 (reserved null-pointer sentinel) + SB, root, leafA, leafB, backup
    # (6) + one data block per file. Block 0 is counted used so the kernel's
    # allocator oracle (which reserves it) agrees with this hint.
    used = 6 + len(objects)
    free_blocks = total_blocks - used
    sb = build_superblock(total_blocks=total_blocks, free_blocks=free_blocks, generation=gen,
                          uuid16=uuidlib.uuid4().bytes, root_block=ROOT_BLOCK, root_csum=root_csum)

    img = bytearray(size_bytes)
    def put(n, b):
        img[n * bs:n * bs + len(b)] = b
    put(SB_BLOCK, sb); put(ROOT_BLOCK, root); put(LEAFA_BLOCK, leafA); put(LEAFB_BLOCK, leafB)
    for b, d in data_blocks:
        put(b, d)
    put(BACKUP_BLOCK, sb)
    with open(path, "wb") as f:
        f.write(img)

    print(f"Wrote {path}: 2-level tree, {total_blocks} blocks of {bs}, free {free_blocks}")
    print(f"  root internal : block {ROOT_BLOCK} (level 1, csum 0x{root_csum:08X}, 2 slots)")
    for (k, p) in slots:
        o, t, off = struct.unpack(L.KEY_FMT, k)
        pb, pc, pg, pf = struct.unpack(L.BLOCK_PTR_FMT, p)
        print(f"    slot {{obj={o},type={t},off=0x{off:08X}}} -> block {pb} (csum 0x{pc:08X})")
    print(f"  leaf A        : block {LEAFA_BLOCK} ({len(leafA_items)} items, csum 0x{leafA_csum:08X})")
    print(f"  leaf B        : block {LEAFB_BLOCK} ({len(leafB_items)} items, csum 0x{leafB_csum:08X})")


if __name__ == "__main__":
    make_image("embkfs.img")            # flat: single leaf (collision regression)
    print()
    make_tree_image("embkfs_tree.img")  # tall: internal root + two leaves