#!/usr/bin/env python3
"""
mkfs_embkfs.py — a minimal EMBKFS formatter (the "mkfs" for EMBKFS).

It writes a known-good EMBKFS image that the EmbLink kernel's read-only mount
path can validate against — the same role mkfs.vfat played for FAT. The image is
deliberately small but exercises the WHOLE format:

    - a primary superblock at byte 65536 (and a backup at the last block)
    - a metadata B-tree rooted at block 17: a single LEAF node (level 0) when
      the items fit in one 4 KiB block, else a level-1 internal root over as
      many leaves as the items need (build_btree auto-splits). The default boot
      image now holds enough files -- the userland ELFs, libembk.so, a real
      font -- to need multiple leaves.
    - inside the leaf/leaves, as key-sorted items:
        * the root directory's inode             {OBJID_ROOT, INODE, 0}
        * the root directory's entries           {OBJID_ROOT, DIR_ENTRY, hash}
          (one item per name hash; names that COLLIDE on the hash share one item
           as a chain of records — see build_dir_entry_items)
                * each file's inode                      {fileid, INODE, 0}
                * each file's extent                     {fileid, EXTENT, 0}
                    (length_blocks may be >1 for larger files)
        - one or more data blocks per file

The default image holds regular files plus one symlink in the root directory:
    hello.txt    — an ordinary entry (its own single-record dir-entry item)
    wgyehkb.txt  } these two share the SAME CRC32C name hash (0xC38842AB), so
    illoeuw.txt  } they land in ONE dir-entry item as a collision chain.
    hello.lnk    — a symlink object whose payload stores "hello.txt"
    plus the userland ELFs / libembk.so / font.ttf when those are built.

This still tests the superblock, node header, leaf layout, field-wise key
ordering, collision chaining, and every item type — and now, once the app set
is packed in, also the multi-leaf + internal-node descent path.

Block map (4 KiB blocks):
    block 16     : primary superblock (byte offset 65536)
    block 17     : the B-tree root (a leaf if it all fits, else a level-1 node)
    block 18..   : the remaining leaves (multi-leaf case), then file data blocks
    last block   : backup superblock
"""

import sys
import os
import glob
import struct
import time
import uuid as uuidlib

from crc32c import crc32c
import layout as L

# v2.2: real wall-clock timestamps for every object this formatter creates,
# instead of the placeholder zeros v2.0/v2.1 shipped with (the kernel had no
# RTC driver until Phase 0 of that work landed). One "now" shared across the
# whole format pass -- every object created by a single mkfs run is stamped
# with the exact same instant, matching how a real filesystem's initial
# population would look (everything created "at format time").
NOW_NS = int(time.time() * 1_000_000_000)

# The libc tcc links against ON the OS -- the SAME archive the cross-build uses,
# so a program built on EmbLink and one built for it are the same program.
#
# The PATH COMES FROM THE MAKEFILE (EMBK_NEWLIB_LIBC, derived from its single
# NEWLIB_PREFIX). Never hardcode a developer's home directory here: NEWLIB_PREFIX
# is `?=` precisely so a second machine can point it elsewhere, and a literal
# path silently defeats that -- mkfs would skip libc.a on a machine that HAS one,
# and `test tcc link` would fail for a reason nothing on screen explains.
# Empty (env unset -> no libc.a packed) is honest: tcc can still compile (-c),
# it just cannot link, which is exactly the truth of that image.
NEWLIB_LIBC = os.environ.get("EMBK_NEWLIB_LIBC", "")

# The HEADER trees tcc searches ON the OS (same Makefile-owns-the-path rule as
# NEWLIB_LIBC above):
#   EMBK_NEWLIB_INC -> /system/abi/include    newlib's headers: declaring the
#                      libc IS part of the ABI contract, so they live with
#                      crt0.o/syscalls.o/libc.a in the sealed region.
#   EMBK_TCC_INC    -> /data/apps/tcc/include tcc's own compiler headers
#                      (stddef.h/stdarg.h/...): they belong to the COMPILER,
#                      so they live with the app.
# Both must mirror what tools/tcc/build-tcc-emblink.sh bakes into tcc's
# sysinclude paths. Empty -> not packed: tcc then compiles only header-free
# programs, which is the truth of that image.
NEWLIB_INC = os.environ.get("EMBK_NEWLIB_INC", "")
TCC_INC    = os.environ.get("EMBK_TCC_INC", "")


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
    """One on-disk block: payload bytes zero-padded to exactly BLOCK_SIZE."""
    if len(data) > L.BLOCK_SIZE:
        raise ValueError("block payload exceeds BLOCK_SIZE")
    buf = bytearray(L.BLOCK_SIZE)
    buf[0:len(data)] = data
    return bytes(buf)


def build_data_blocks(data: bytes) -> list:
    """Split file bytes into 4 KiB blocks (last block is zero-padded)."""
    if len(data) == 0:
        return []

    blocks = []
    for off in range(0, len(data), L.BLOCK_SIZE):
        blocks.append(build_data_block(data[off:off + L.BLOCK_SIZE]))
    return blocks


# An extent's checksum covers the WHOLE extent, so NOTHING in it can be served
# until ALL of it has been read and CRC'd. One extent per file therefore made a
# single 8 KB read cost O(filesize): the 10.3 MB python314.zip cost 443% read
# amplification cold (measured -- `test ioperf`), and a 2 GB file would mean
# reading and CRC'ing 2 GB before byte 0. Capping extent size makes that cost
# BOUNDED and independent of file size, which is the whole point.
#
# MUST be a multiple of BLOCK_SIZE: chunks start on block boundaries, and the
# disk_block of chunk N is derived as blk + offset // BLOCK_SIZE.
#
# WHY 1 MiB, AND WHY NOT SMALLER -- this was A/B'd. Don't re-tune it from theory.
#
# Read amplification for extent size E against the kernel's read-ahead window W
# (EMBKFS_WCACHE_WIN, 64 KiB) is (2E - W)/E: a first touch verifies ALL of E but
# copies out only W, then the remaining E/W - 1 windows re-read those same blocks.
# Predicted 193.75% at E=1MiB, measured 194% -- the model is exact. It also says
# E <= W would give 100%, and E=64KiB duly measured 101%.
#
# SHIPPING 64 KiB ON THAT REASONING WOULD HAVE BEEN A MISTAKE. Same kernel,
# `test posix`, only this constant changed:
#
#                    device reads   B-tree node reads   time in dev->read
#      E = 64 KiB           8535                8336            10860 ms
#      E =  1 MiB           5401                5152             5886 ms
#
# The entire +3134 device reads is +3184 B-TREE NODE READS. ~95% of all device
# reads on that workload are tree nodes, and more extents = more items = a bigger
# tree = every COW rebuild (70 mkdirs there) reads more of it. Buying cold read
# amplification 194% -> 101% cost ~85% MORE device time system-wide: the read path
# is not the B-tree's only customer.
#
# (`ecache 48 hit / 1 miss` and `23 prefix calls` came out IDENTICAL under both,
# which is what killed the "ecache thrashes" and "prefix rescans" explanations.)
#
# So: 1 MiB. It removes the O(filesize) pathology (443% -> 194%), keeps the tree
# small, and a 2 GB file needs 2048 extents / ~98 KB of cached extent map instead
# of 32768 / ~1.5 MB. Revisit only WITH NUMBERS from `test posix` + `test ioperf`.
EXTENT_MAX_BYTES = 1024 * 1024
assert EXTENT_MAX_BYTES % L.BLOCK_SIZE == 0


def build_extent_items(oid: int, blk: int, data: bytes, gen: int) -> list:
    """Extent items for one file, capped at EXTENT_MAX_BYTES each.

    The kernel takes an extent's LOGICAL offset from the ITEM KEY
    (`out[*n].offset = it->key.offset` in embkfs_collect_extents), not from the
    packed struct -- which is why the key's third field is the file offset here.

    Satisfies the kernel's two checks verbatim:
      - embkfs_extent_validate: length != 0, logical_size != 0 and
        logical_size <= length * BLOCK_SIZE.
      - embkfs_validate_extent_map: extents contiguous from offset 0, each
        starting exactly where the previous ended, summing to the inode size.
    Empty files yield no extents (range() is empty), matching the old
    `if nblocks > 0` guard.
    """
    out = []
    for off in range(0, len(data), EXTENT_MAX_BYTES):
        chunk = data[off:off + EXTENT_MAX_BYTES]
        out.append((L.pack_key(oid, L.EMBK_TYPE_EXTENT, off),
                    L.pack_extent(disk_block=blk + off // L.BLOCK_SIZE,
                                  length_blocks=(len(chunk) + L.BLOCK_SIZE - 1) // L.BLOCK_SIZE,
                                  logical_size=len(chunk),
                                  data_checksum=crc32c(chunk), generation=gen)))
    return out


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


def make_image(path: str, size_bytes: int = 64 * 1024 * 1024, objects=None):
    # 64 MB (was 8, was 4). Each bump had the same trigger: one binary grew
    # past the volume and free_blocks went NEGATIVE -- struct.pack('Q') then
    # refuses it, which is the (cryptic) way this failure announces itself.
    #   4 -> 8 MB: shell.elf (~410 KB static-newlib)
    #   8 -> 64 MB: cxxdemo.elf is ~9.4 MB ON ITS OWN -- <iostream> pulls in
    #               locales + the whole ios/facet machinery (it was 903 KB
    #               before that one #include). CPython will be bigger again.
    # The kernel reads total_blocks from the superblock, so growing is safe;
    # the image is sparse-ish on disk and QEMU only reads what's used.
    bs = L.BLOCK_SIZE
    total_blocks = size_bytes // bs
    gen = 1

    # --- fixed block assignments ---
    SB_BLOCK     = L.SUPERBLOCK_OFFSET // bs    # 65536/4096 = block 16
    LEAF_BLOCK   = SB_BLOCK + 1                 # block 17: B-tree root
    BACKUP_BLOCK = total_blocks - 1            # last block: backup superblock

    # AUTO-DISCOVER the userland: init.elf + fixtures + every build/*.elf +
    # libembk.so + the font. Adding a new app needs no edit here (see
    # discover_userland_objects). A caller may override `objects` to bake a
    # DIFFERENT tree (make_dirtree_image does, for the nested-directory fixture);
    # everything downstream is name-agnostic, so nested names just work.
    if objects is None:
        objects = discover_userland_objects()

    # --- build the metadata B-tree (auto multi-leaf) ---
    # Root lives at META_START (the block right after the superblock); file data
    # follows every metadata block. How many metadata blocks the tree needs
    # depends only on item sizes, NOT on where data blocks land -- an extent's
    # disk_block is a fixed-width field, so shifting the data region never
    # changes leaf packing. So: build once with a provisional data start to
    # learn the metadata-block count, place data right after it, then rebuild
    # for real (the packing comes out identical, asserted below).
    META_START = LEAF_BLOCK                        # block 17: root of the tree
    prov_items, _, _, _ = build_root_items(objects, gen, data_start=META_START + 1)
    _, _, _, n_meta = build_btree(prov_items, gen, META_START)

    data_start = META_START + n_meta
    items, data_blocks, file_layouts, _ = build_root_items(objects, gen, data_start)
    root_block, root_csum, meta_blocks, n_meta2 = build_btree(items, gen, META_START)
    assert n_meta2 == n_meta, "metadata block count changed between passes"

    # free_blocks hint: block 0 (reserved null-pointer sentinel) + superblock +
    # every metadata block (root + leaves) + backup superblock, plus all file
    # data blocks. Block 0 is never written but counted used so the kernel's
    # allocator oracle (which reserves it) agrees with this hint.
    used = 3 + n_meta + sum(nblocks for (_n, _s, nblocks, _z, _c) in file_layouts)
    free_blocks = total_blocks - used

    superblock = build_superblock(total_blocks=total_blocks,
                                  free_blocks=free_blocks, generation=gen,
                                  uuid16=uuidlib.uuid4().bytes,
                                  root_block=root_block, root_csum=root_csum)

    # --- assemble the full image ---
    img = bytearray(size_bytes)  # all zeros

    def put(block_no, block_bytes):
        off = block_no * bs
        img[off:off + len(block_bytes)] = block_bytes

    put(SB_BLOCK, superblock)
    for b, d in meta_blocks:            # root + leaves
        put(b, d)
    for b, d in data_blocks:
        put(b, d)
    put(BACKUP_BLOCK, superblock)       # backup is an identical copy

    with open(path, "wb") as f:
        f.write(img)

    # --- report ---
    n_leaves = n_meta - 1 if n_meta > 1 else 1
    print(f"Wrote {path}  ({size_bytes} bytes, {total_blocks} blocks of {bs})")
    print(f"  superblock      : block {SB_BLOCK} (byte {SB_BLOCK*bs})")
    if n_meta == 1:
        print(f"  metadata        : single leaf at block {root_block} "
              f"(csum 0x{root_csum:08X}, {len(items)} items)")
    else:
        print(f"  metadata        : level-1 root at block {root_block} "
              f"(csum 0x{root_csum:08X}) over {n_leaves} leaves "
              f"(blocks {META_START + 1}..{META_START + n_leaves}), {len(items)} items total")
    for name, start, nblocks, logical_size, data_csum in file_layouts:
        where = f"block {start}" if nblocks == 1 else f"blocks {start}..{start + nblocks - 1}"
        # n_ext, not a whole-file checksum: with bounded extents the image stores
        # one checksum PER EXTENT, so a single file-wide csum corresponds to
        # nothing on disk. Printing it as though it did is how you get someone
        # verifying against a field that isn't there. It stays as a build-time
        # fingerprint of the source bytes, labelled as such.
        n_ext = (logical_size + EXTENT_MAX_BYTES - 1) // EXTENT_MAX_BYTES
        print(f"  file data       : {where}  (\"{name.decode()}\", {logical_size} bytes,"
              f" {nblocks} blocks, {n_ext} extent{'s' if n_ext != 1 else ''},"
              f" src fingerprint 0x{data_csum:08X})")
    print(f"  backup superblk : block {BACKUP_BLOCK} (last block)")
    print(f"  free_blocks hint: {free_blocks}")


def derive_xts_keys(passphrase: bytes, salt: bytes, iterations: int):
    """PBKDF2-HMAC-SHA256, 64 bytes out: [0:32) data key, [32:64) tweak key.
    Must match kernel/fs/embkfs/embkfs.c's embkfs_try_unlock() exactly."""
    import hashlib
    keymat = hashlib.pbkdf2_hmac("sha256", passphrase, salt, iterations, 64)
    return keymat[:32], keymat[32:]


def xts_encrypt_block(data_key: bytes, tweak_key: bytes, block_number: int, plaintext: bytes) -> bytes:
    """AES-256-XTS over exactly one on-disk filesystem block, tweak = the
    block's own disk address as a little-endian uint64 in a 16-byte buffer
    (high 8 bytes zero). Must match kernel/crypto/xts.c's
    block_number_to_tweak_input() + aes_xts_encrypt() exactly -- this IS
    the independent oracle for that code, not a copy of it."""
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
    tweak = block_number.to_bytes(8, "little") + b"\x00" * 8
    c = Cipher(algorithms.AES(data_key + tweak_key), modes.XTS(tweak))
    e = c.encryptor()
    return e.update(plaintext) + e.finalize()


def build_crypto_header(passphrase: bytes, iterations: int = 200_000):
    """Returns (header_bytes, data_key, tweak_key). A fresh random salt
    every call, like a real mkfs would use."""
    import os
    salt = os.urandom(16)
    data_key, tweak_key = derive_xts_keys(passphrase, salt, iterations)
    check = xts_encrypt_block(data_key, tweak_key, 0, L.KEY_CHECK_PLAINTEXT)
    hdr = struct.pack(L.CRYPTO_HEADER_FMT, L.EMBKFS_CRYPTO_HEADER_MAGIC,
                      salt, iterations, check, b"\x00" * 8)
    assert len(hdr) == L.CRYPTO_HEADER_SIZE
    return hdr, data_key, tweak_key


def make_encrypted_image(path: str, passphrase: bytes, size_bytes: int = 1024 * 1024,
                         iterations: int = 10_000):
    """A minimal ENCRYPTED EMBKFS image: superblock with the ENCRYPTED
    incompat bit + crypto header, root dir, and one file whose data block
    is genuinely XTS-encrypted on disk -- exercises the kernel's real
    mount-time unlock + per-block decrypt path, not just the crypto
    header's own key-check. `iterations` defaults low (this is a TEST
    fixture, unlock speed during automated boot tests matters more than realistic
    KDF cost here; real-world formatting should use build_crypto_header's
    default of 200_000)."""
    bs = L.BLOCK_SIZE
    total_blocks = size_bytes // bs
    gen = 1

    SB_BLOCK     = L.SUPERBLOCK_OFFSET // bs
    LEAF_BLOCK   = SB_BLOCK + 1
    DATA_BLOCK   = SB_BLOCK + 2
    BACKUP_BLOCK = total_blocks - 1

    crypto_hdr, data_key, tweak_key = build_crypto_header(passphrase, iterations)

    plaintext = b"The vault is open. EMBKFS-v2.2 Phase 4 encryption works.\n"

    items = []
    items.append((L.pack_key(L.OBJID_ROOT, L.EMBK_TYPE_INODE, 0),
                  L.pack_inode(size=0, blocks=0, links=2,
                               mode=L.S_IFDIR | L.PERM_DIR, uid=0, gid=0,
                               atime=NOW_NS, mtime=NOW_NS, ctime=NOW_NS, btime=NOW_NS,
                               generation=gen)))

    oid = L.FIRST_USER_OBJID
    nblocks = (len(plaintext) + bs - 1) // bs
    items.append((L.pack_key(oid, L.EMBK_TYPE_INODE, 0),
                  L.pack_inode(size=len(plaintext), blocks=nblocks, links=1,
                               mode=L.S_IFREG | L.PERM_FILE, uid=0, gid=0,
                               atime=NOW_NS, mtime=NOW_NS, ctime=NOW_NS, btime=NOW_NS,
                               generation=gen)))

    # Encrypt each on-disk block independently (its own tweak = its own
    # disk block number), then checksum the CIPHERTEXT -- matching the
    # kernel's write-time order (encrypt, THEN checksum) and its
    # encrypted-extent convention of hashing the WHOLE block_size per
    # block, padding included, not just the logical/real byte count.
    plain_blocks = build_data_blocks(plaintext)
    cipher_blocks = []
    csum = 0
    for i, pb in enumerate(plain_blocks):
        cb = xts_encrypt_block(data_key, tweak_key, DATA_BLOCK + i, pb)
        assert len(cb) == bs
        cipher_blocks.append(cb)
        csum = crc32c(cb, csum)

    items.append((L.pack_key(oid, L.EMBK_TYPE_EXTENT, 0),
                  L.pack_extent(disk_block=DATA_BLOCK, length_blocks=nblocks,
                                logical_size=len(plaintext),
                                data_checksum=csum, generation=gen,
                                flags=L.EXTENT_FLAG_ENCRYPTED)))

    items.extend(build_dir_entry_items(L.OBJID_ROOT, [(b"secret.txt", oid, L.DT_REG)]))
    items.sort(key=lambda it: struct.unpack(L.KEY_FMT, it[0]))

    leaf = build_leaf_block(generation=gen, block_no=LEAF_BLOCK, items=items)
    leaf_csum = crc32c(leaf[8:])

    used = 4 + nblocks
    free_blocks = total_blocks - used

    root_ptr = L.pack_block_ptr(block=LEAF_BLOCK, checksum=leaf_csum, generation=gen, flags=0)
    checkpoint_ptr = L.null_block_ptr()
    body = L.pack_superblock_body(
        block_size=L.BLOCK_SIZE, total_blocks=total_blocks, free_blocks=free_blocks,
        uuid16=uuidlib.uuid4().bytes, generation=gen,
        root_ptr32=root_ptr, checkpoint_ptr32=checkpoint_ptr,
        feat_incompat=L.EMBKFS_INCOMPAT_ENCRYPTED)
    sb_csum = crc32c(body)
    sb_bytes = body + struct.pack("<Q", sb_csum)
    assert len(sb_bytes) == L.SUPERBLOCK_SIZE

    sb_block = bytearray(L.BLOCK_SIZE)
    sb_block[0:len(sb_bytes)] = sb_bytes
    sb_block[L.CRYPTO_HEADER_OFFSET:L.CRYPTO_HEADER_OFFSET + len(crypto_hdr)] = crypto_hdr
    superblock = bytes(sb_block)

    img = bytearray(size_bytes)

    def put(block_no, block_bytes):
        off = block_no * bs
        img[off:off + len(block_bytes)] = block_bytes

    put(SB_BLOCK, superblock)
    put(LEAF_BLOCK, leaf)
    for i, cb in enumerate(cipher_blocks):
        put(DATA_BLOCK + i, cb)
    put(BACKUP_BLOCK, superblock)

    with open(path, "wb") as f:
        f.write(img)

    print(f"Wrote {path}  ({size_bytes} bytes, {total_blocks} blocks of {bs}) -- ENCRYPTED")
    print(f"  passphrase        : <redacted> ({len(passphrase)} bytes)")
    print(f"  kdf_iterations    : {iterations}")
    print(f"  file data (cipher): blocks {DATA_BLOCK}..{DATA_BLOCK + nblocks - 1}  "
          f"(\"secret.txt\", {len(plaintext)} logical bytes, {nblocks} block(s), "
          f"ciphertext csum 0x{csum:08X})")
    print(f"  free_blocks hint  : {free_blocks}")


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


# --- generic B-tree assembly (auto multi-leaf) -----------------------------
# A leaf's payload budget (spec §8.3): NODE_HDR(40) then, for each item, an
# ITEM_HDR(32) growing down + its data growing up, all inside one BLOCK_SIZE.
# So a leaf holds items while sum(ITEM_HDR + len(data)) <= BLOCK_SIZE - NODE_HDR.
LEAF_ITEM_BUDGET  = L.BLOCK_SIZE - L.NODE_HDR_SIZE            # 4056 bytes
INTERNAL_SLOT_MAX = (L.BLOCK_SIZE - L.NODE_HDR_SIZE) // SLOT  # 72 leaves per internal node


def pack_items_into_leaves(items):
    """Greedily pack KEY-SORTED `items` into as few leaves as each fits in one
    block. Returns a list of item-lists, one per leaf, preserving global key
    order (so each leaf owns a contiguous key range -- exactly what an internal
    node's per-leaf routing slots need)."""
    leaves, cur, used = [], [], 0
    for key, data in items:
        cost = L.ITEM_HDR_SIZE + len(data)
        if cost > LEAF_ITEM_BUDGET:
            raise ValueError(f"single item too large for a leaf: {cost} > {LEAF_ITEM_BUDGET}")
        if cur and used + cost > LEAF_ITEM_BUDGET:
            leaves.append(cur)
            cur, used = [], 0
        cur.append((key, data))
        used += cost
    if cur:
        leaves.append(cur)
    return leaves


def build_btree(items, gen, first_block, forced_leaf_splits=None):
    """Assemble a B-tree over KEY-SORTED `items`, with the ROOT always at
    `first_block` (so the superblock's root pointer is stable regardless of how
    many leaves the items need):
      - one leaf   -> `first_block` IS the level-0 leaf.
      - many leaves -> `first_block` is a level-1 internal root; the leaves
        follow at first_block+1, first_block+2, ...
    This is what lets the boot image (make_image) hold an arbitrary number of
    files without overflowing a single 4 KiB leaf -- the leaves auto-split.

    `forced_leaf_splits`: pass an explicit list-of-item-lists to bypass the
    greedy packer and force a specific leaf layout (make_tree_image uses this
    to pin a 2-leaf split as a fixed descent-path regression fixture).

    Returns (root_block, root_csum, meta_blocks, n_meta_blocks) where
    meta_blocks is a list of (block_no, block_bytes) to write to the image."""
    leaf_lists = forced_leaf_splits if forced_leaf_splits is not None \
        else pack_items_into_leaves(items)

    if len(leaf_lists) == 1:
        leaf = build_leaf_block(gen, first_block, leaf_lists[0])
        return first_block, crc32c(leaf[8:]), [(first_block, leaf)], 1

    if len(leaf_lists) > INTERNAL_SLOT_MAX:
        raise ValueError(f"{len(leaf_lists)} leaves exceed one internal node "
                         f"({INTERNAL_SLOT_MAX} slots); a level-2 tree is not "
                         f"implemented -- add it here if an image ever needs it")

    meta_blocks, slots = [], []
    leaf_block = first_block + 1                       # leaves live above the root
    for il in leaf_lists:
        leaf = build_leaf_block(gen, leaf_block, il)
        leaf_csum = crc32c(leaf[8:])
        meta_blocks.append((leaf_block, leaf))
        # slot key = smallest key in that child's subtree = its first item's key
        slots.append((il[0][0], L.pack_block_ptr(leaf_block, leaf_csum, gen)))
        leaf_block += 1
    root = build_internal_block(gen, first_block, level=1, slots=slots)
    root_csum = crc32c(root[8:])
    meta_blocks.insert(0, (first_block, root))
    return first_block, root_csum, meta_blocks, len(meta_blocks)


# Small on-disk fixtures always present regardless of what's been built: a
# plain regression entry, a name-hash collision chain (wgyehkb/illoeuw share
# CRC32C hash 0xC38842AB -> ONE dir-entry item), and a symlink.
FIXTURE_OBJECTS = [
    (b"hello.txt",   L.DT_REG, L.S_IFREG | L.PERM_FILE,
     b"Hello from EMBKFS! This file was written by mkfs_embkfs.py.\n"),
    (b"wgyehkb.txt", L.DT_REG, L.S_IFREG | L.PERM_FILE,
     b"Colliding file A: wgyehkb.txt (hash 0xC38842AB, shared with illoeuw.txt).\n"),
    (b"illoeuw.txt", L.DT_REG, L.S_IFREG | L.PERM_FILE,
     b"Colliding file B: illoeuw.txt (hash 0xC38842AB, shared with wgyehkb.txt).\n"),
    (b"hello.lnk",   L.DT_LNK, L.S_IFLNK | L.PERM_LNK, b"hello.txt"),
]


def _read_file(path):
    try:
        with open(path, "rb") as f:
            return f.read()
    except FileNotFoundError:
        return None


def _tree_objects(host_dir: str, image_prefix: bytes, suffix: str = ""):
    """Walk a HOST directory tree and emit (name, dtype, mode, data) objects
    placing every file under `image_prefix` on the image, preserving relative
    paths (build_root_items auto-creates the intermediate dirs from the
    '/'-joined names). `suffix` filters by extension (e.g. ".h") so a header
    pack can't accidentally drag in stray build residue. Sorted for a
    deterministic image. Empty/missing host_dir -> no objects (honest: the
    capability is absent from that image, not faked)."""
    out = []
    if not host_dir or not os.path.isdir(host_dir):
        return out
    for root, dirs, files in os.walk(host_dir):
        dirs.sort()
        for fn in sorted(files):
            if suffix and not fn.endswith(suffix):
                continue
            full = os.path.join(root, fn)
            rel = os.path.relpath(full, host_dir).replace(os.sep, "/")
            data = _read_file(full)
            if data is not None:
                out.append((image_prefix + rel.encode(), L.DT_REG,
                            L.S_IFREG | L.PERM_FILE, data))
    return out


# --- the userspace LAYOUT (docs/USERSPACE.md §6) --------------------------
# The boot image is a TREE, not a flat root. Placement follows the sealed/mutable
# boundary (D2 §3): the system's own programs + the ABI are sealed under /system;
# installed applications (every compiler, the sval tools) are mutable state under
# /data/apps/<name>/. Everything build_root_items can nest, so this is just a
# name->path map applied at pack time.
#
# Only the system's own programs are sealed under /system/bin; EVERY other *.elf
# is an installed application and lives in /data/apps/<name>/ (D2 §3.2: "all
# installed applications" -- the EmUI demos, the sval tools, the compilers, the
# test binaries). Fonts and the format fixtures are NOT *.elf and are placed
# separately (fonts at root, fixtures at root -- they test the format, not the
# layout).
_SYSTEM_BIN = {"init.elf", "shell.elf", "home.elf"}          # -> /system/bin/

def _elf_dest(name: str) -> bytes:
    """Tree path (bytes, no leading slash) for a packed *.elf basename."""
    if name in _SYSTEM_BIN:
        return b"system/bin/" + name.encode()
    return f"data/apps/{name[:-4]}/{name}".encode()          # every other app: /data/apps/<name>/


def discover_userland_objects(build_dir="build"):
    """Assemble the boot image's objects by DISCOVERING what's been built, placed
    into the docs/USERSPACE.md tree (see _elf_dest / the sealed-vs-mutable map
    above). Adding a new EmUI app still needs no mkfs edit -- drop
    user/bin/foo.c in and foo.elf appears at the root; name it in _SYSTEM_BIN or
    _APPS only if it belongs elsewhere. Returns a list of (name, dtype, mode,
    data); `name` may contain '/', and build_root_items creates the dirs.
      - system programs      -> /system/bin/
      - the shared toolkit   -> /system/lib/libembk.so
      - the ABI (crt0/syscalls/libc.a) -> /system/abi/
      - installed apps       -> /data/apps/<name>/
      - demos, fonts, fixtures -> root (unmoved, see above)
      - empty /data/tmp and /data/users/teo."""
    init = _read_file(f"{build_dir}/init.elf")
    if init is None:
        raise SystemExit(f"mkfs: {build_dir}/init.elf not found -- run `make` first")
    objects = [(_elf_dest("init.elf"), L.DT_REG, L.S_IFREG | 0o755, init)]
    objects.extend(FIXTURE_OBJECTS)                           # format fixtures, at root
    for elf in sorted(glob.glob(f"{build_dir}/*.elf")):       # sorted -> deterministic image
        name = os.path.basename(elf)
        if name == "init.elf":
            continue                                          # already added first
        objects.append((_elf_dest(name), L.DT_REG, L.S_IFREG | 0o755, _read_file(elf)))
    so = _read_file(f"{build_dir}/libembk.so")
    if so is not None:
        objects.append((b"system/lib/libembk.so", L.DT_REG, L.S_IFREG | 0o755, so))
    font = _read_file("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf")
    if font is not None:
        objects.append((b"font.ttf", L.DT_REG, L.S_IFREG | L.PERM_FILE, font))
    # The terminal's MONOSPACE face (same DejaVu family, so the same
    # rasterizer tech) -- shell tables align only in fixed-pitch glyphs.
    mono = _read_file("/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf")
    if mono is not None:
        objects.append((b"mono.ttf", L.DT_REG, L.S_IFREG | L.PERM_FILE, mono))
    # THE ABI, sealed under /system/abi (docs/USERSPACE.md D2 §3.1): crt0.o
    # (_start), syscalls.o (the newlib retargeting layer) and libc.a ARE the
    # definition of "targeting EmbLinkOS". tcc READS them (read-only reach into
    # the sealed region) and links with `-L/system/abi`; it is an app in /data,
    # not part of the contract. libc.a (~6.6 MB) is the biggest object on the
    # image and earns it: without it `tcc t.c -o t.elf` cannot resolve exit().
    for tc in ("crt0.o", "syscalls.o"):
        blob = _read_file(f"{build_dir}/{tc}")
        if blob is not None:
            objects.append((b"system/abi/" + tc.encode(), L.DT_REG, L.S_IFREG | L.PERM_FILE, blob))
    libc = _read_file(NEWLIB_LIBC) if NEWLIB_LIBC else None
    if libc is not None:
        objects.append((b"system/abi/libc.a", L.DT_REG, L.S_IFREG | L.PERM_FILE, libc))

    # THE HEADERS (the other half of "targeting EmbLinkOS"): newlib's include
    # tree under /system/abi/include (declaring the libc is part of the sealed
    # ABI contract, next to the objects that implement it), and tcc's own
    # compiler headers (stddef.h/stdarg.h/...) under /data/apps/tcc/include
    # (they belong to the COMPILER, so they live with the app). These two paths
    # are exactly tcc's baked-in sysinclude search order
    # (tools/tcc/build-tcc-emblink.sh) -- with them packed, `#include <stdio.h>`
    # resolves ON the OS and tcc can compile real programs, not just
    # header-free toys. ~140 files, ~1.1 MB.
    objects.extend(_tree_objects(NEWLIB_INC, b"system/abi/include/", ".h"))
    objects.extend(_tree_objects(TCC_INC, b"data/apps/tcc/include/", ".h"))

    # SOURCE on the image: tally's exact closure (the reference pipeline
    # consumer + the sval SDK it links), preserved with its tree shape so the
    # quote-includes ("sval/sval.h", "value/value.h") resolve with a single
    # -I/data/src/shell -- the same layout the host build compiles with
    # (SHELL_INC = -Ishell). This is what `test tcc tally` rebuilds ON the OS
    # and installs over /data/apps/tally/tally.elf: the first component of the
    # system rebuilt by the system. An explicit list, not a tree walk -- the
    # closure is the point (7 files, ~1400 lines), not the whole shell/.
    _TALLY_SRC = ("tools/tally.c", "sval/sval.c", "sval/sval.h",
                  "value/value.c", "value/value.h",
                  "wire/wire.c", "wire/wire.h")
    for rel in _TALLY_SRC:
        blob = _read_file(f"shell/{rel}")
        if blob is not None:
            objects.append((b"data/src/shell/" + rel.encode(),
                            L.DT_REG, L.S_IFREG | L.PERM_FILE, blob))

    # EmbBuild's world (docs/BUILD.md): the tally PROJECT (its source + the
    # first manifest) at /data/src/tally/, and EmbBuild's own source at
    # /data/src/embbuild/ -- the precondition for target #3, `embbuild
    # embbuild`. tally.c appears in BOTH /data/src/shell (the test tcc tally
    # closure, unchanged) and /data/src/tally (the manifest's project) -- a
    # 2 KB duplication kept so the existing green test stays untouched;
    # collapse when embbuild subsumes the hand-rolled test.
    for host, image in (("shell/tools/tally.c",            b"data/src/tally/tally.c"),
                        ("shell/tools/tally.build.ebm",    b"data/src/tally/build.ebm"),
                        ("shell/tools/sysinfo.c",          b"data/src/sysinfo/sysinfo.c"),
                        ("shell/tools/sysinfo.build.ebm",  b"data/src/sysinfo/build.ebm"),
                        ("shell/tools/embbuild.c",         b"data/src/embbuild/embbuild.c"),
                        ("shell/tools/embbuild.build.ebm", b"data/src/embbuild/build.ebm"),
                        # embk.h + embk_syscall.h: the ABI's userspace surface
                        # -- sysinfo.c and embbuild.c #include them, so
                        # declaring the ABI joins the ABI (BUILD.md's rule).
                        ("user/lib/embk.h",                b"system/abi/include/embk.h"),
                        ("user/lib/embk_syscall.h",        b"system/abi/include/embk_syscall.h")):
        blob = _read_file(host)
        if blob is not None:
            objects.append((image, L.DT_REG, L.S_IFREG | L.PERM_FILE, blob))

    # CPython's stdlib zip + ._pth live WITH the interpreter under /data/apps/
    # python/. The ._pth holds a RELATIVE name ("python314.zip"), joined onto the
    # executable's OWN directory -- so moving all three together needs no ._pth
    # edit; getpath resolves the zip to /data/apps/python/python314.zip.
    pyzip = _read_file(f"{build_dir}/python314.zip")
    if pyzip is not None:
        objects.append((b"data/apps/python/python314.zip", L.DT_REG, L.S_IFREG | L.PERM_FILE, pyzip))
    pth = _read_file(f"{build_dir}/python.elf._pth")
    if pth is not None:
        objects.append((b"data/apps/python/python.elf._pth", L.DT_REG, L.S_IFREG | L.PERM_FILE, pth))

    # Empty user/scratch directories the layout commits to now (D4 §5, D3 §4.1),
    # so a session can chdir into a home and tcc has a scratch dir to write to.
    objects.append((b"data/tmp",        L.DT_DIR, L.S_IFDIR | L.PERM_DIR, None))
    objects.append((b"data/users/teo",  L.DT_DIR, L.S_IFDIR | L.PERM_DIR, None))
    return objects


def build_root_items(objects, gen, data_start):
    """Build the key-sorted metadata items + data-block list + file layout for a
    directory TREE holding `objects` (each `(name, dtype, mode, data)`).

    `name` may contain '/' to place the object in a subdirectory; EVERY
    intermediate directory is created automatically (S_IFDIR | PERM_DIR). A
    caller can also pass an explicit directory object (dtype == DT_DIR, data
    ignored) to create an EMPTY directory -- e.g. /data/tmp with nothing in it.

    Backward compatibility is the contract: an all-flat object list (no '/' in
    any name) produces BYTE-IDENTICAL output to the pre-directory formatter --
    the root inode is the only directory, file oids run FIRST_USER_OBJID.. in
    object order, and every entry lands in the root. That is why the default
    boot image is unaffected until the layout migration commit changes the names.

    Returns (items_sorted, data_blocks, file_layouts, dir_entries)."""
    def split_path(name: bytes):
        return tuple(p for p in name.split(b"/") if p)   # drop empty (// or leading /)

    # --- discover the tree, PRESERVING object order for oid + block assignment
    # so the flat case is unchanged. A node's identity is its component tuple;
    # the root is the empty tuple (). ---
    file_objs = []              # (comps, dtype, mode, data) for DT_REG/DT_LNK, in order
    dir_mode = {(): L.S_IFDIR | L.PERM_DIR}   # component-tuple -> mode (root default)
    all_dirs = {()}            # every directory that must exist (root + ancestors + explicit)
    order = []                 # component tuples in CALLER order (for flat-compatible oids)

    for name, dtype, mode, data in objects:
        comps = split_path(name)
        if not comps:
            raise SystemExit("mkfs: empty object name")
        order.append(comps)
        if dtype == L.DT_DIR:
            dir_mode[comps] = mode
            all_dirs.add(comps)
        else:
            file_objs.append((comps, dtype, mode, data))
        for i in range(1, len(comps)):          # every ancestor is a directory
            all_dirs.add(comps[:i])

    # --- assign oids: caller objects in ORDER first (flat byte-identity), then
    # any auto-created intermediate dirs (sorted, deterministic). ---
    oid_of = {(): L.OBJID_ROOT}
    next_oid = L.FIRST_USER_OBJID
    for comps in order:
        oid_of[comps] = next_oid
        next_oid += 1
    for comps in sorted(all_dirs - set(order) - {()}):
        oid_of[comps] = next_oid
        next_oid += 1

    # --- parent -> [(leaf, child_oid, dtype)] for every non-root node ---
    children = {d: [] for d in all_dirs}
    for comps in order:                          # caller objects (files, links, explicit dirs)
        dtype = L.DT_DIR if comps in dir_mode else next(
            (dt for (c, dt, _m, _d) in file_objs if c == comps), L.DT_REG)
        children[comps[:-1]].append((comps[-1], oid_of[comps], dtype))
    for comps in (all_dirs - set(order) - {()}):  # auto-created intermediate dirs
        children[comps[:-1]].append((comps[-1], oid_of[comps], L.DT_DIR))

    items, dir_entries, data_blocks, file_layouts = [], [], [], []

    # --- directory inodes + their entry items ---
    for comps in sorted(all_dirs):
        oid = oid_of[comps]
        subdirs = sum(1 for (_n, _o, dt) in children[comps] if dt == L.DT_DIR)
        items.append((L.pack_key(oid, L.EMBK_TYPE_INODE, 0),
                      L.pack_inode(size=0, blocks=0, links=2 + subdirs,
                                   mode=dir_mode.get(comps, L.S_IFDIR | L.PERM_DIR),
                                   uid=0, gid=0,
                                   atime=NOW_NS, mtime=NOW_NS, ctime=NOW_NS, btime=NOW_NS,
                                   generation=gen)))
        items.extend(build_dir_entry_items(oid, children[comps]))
        if comps == ():
            dir_entries = children[comps]        # returned for compat (unused by callers)

    # --- file/symlink inodes, extents, data (object order -> contiguous blocks) ---
    blk = data_start
    for comps, dtype, mode, data in file_objs:
        oid = oid_of[comps]
        nblocks = (len(data) + L.BLOCK_SIZE - 1) // L.BLOCK_SIZE
        data_csum = crc32c(data)
        items.append((L.pack_key(oid, L.EMBK_TYPE_INODE, 0),
                      L.pack_inode(size=len(data), blocks=nblocks, links=1, mode=mode,
                                   uid=0, gid=0,
                                   atime=NOW_NS, mtime=NOW_NS, ctime=NOW_NS, btime=NOW_NS,
                                   generation=gen)))
        items.extend(build_extent_items(oid, blk, data, gen))
        for i, db in enumerate(build_data_blocks(data)):
            data_blocks.append((blk + i, db))
        file_layouts.append((b"/".join(comps), blk, nblocks, len(data), data_csum))
        blk += nblocks

    # FIELD-WISE integer key order (object_id, type, offset), matching the
    # kernel's embk_key_cmp -- not a memcmp of the raw little-endian key bytes.
    items.sort(key=lambda it: struct.unpack(L.KEY_FMT, it[0]))
    return items, data_blocks, file_layouts, dir_entries


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

    with open("build/init.elf", "rb") as f:
        init_elf_data = f.read()

    objects = [
        (b"init.elf",   L.DT_REG, L.S_IFREG | 0o755,
         init_elf_data),
        (b"hello.txt",   L.DT_REG, L.S_IFREG | L.PERM_FILE,
         b"Hello from EMBKFS! This file was written by mkfs_embkfs.py.\n"),
        # wgyehkb.txt and illoeuw.txt share the SAME CRC32C name hash
        # (0xC38842AB) -> one dir-entry item, a 2-record collision chain.
        (b"wgyehkb.txt", L.DT_REG, L.S_IFREG | L.PERM_FILE,
         b"Colliding file A: wgyehkb.txt (hash 0xC38842AB, shared with illoeuw.txt).\n"),
        (b"illoeuw.txt", L.DT_REG, L.S_IFREG | L.PERM_FILE,
         b"Colliding file B: illoeuw.txt (hash 0xC38842AB, shared with wgyehkb.txt).\n"),
        (b"hello.lnk",   L.DT_LNK, L.S_IFLNK | L.PERM_LNK,
         b"hello.txt"),
    ]

    items = []
    items.append((L.pack_key(L.OBJID_ROOT, L.EMBK_TYPE_INODE, 0),
                  L.pack_inode(size=0, blocks=0, links=2, mode=L.S_IFDIR | L.PERM_DIR,
                               uid=0, gid=0,
                               atime=NOW_NS, mtime=NOW_NS, ctime=NOW_NS, btime=NOW_NS,
                               generation=gen)))
    dir_entries, data_blocks, file_layouts = [], [], []
    oid, blk = L.FIRST_USER_OBJID, DATA_START
    for name, dtype, mode, data in objects:
        dir_entries.append((name, oid, dtype))

        nblocks = (len(data) + L.BLOCK_SIZE - 1) // L.BLOCK_SIZE
        data_csum = crc32c(data)

        items.append((L.pack_key(oid, L.EMBK_TYPE_INODE, 0),
                      L.pack_inode(size=len(data), blocks=nblocks, links=1, mode=mode,
                                   uid=0, gid=0,
                                   atime=NOW_NS, mtime=NOW_NS, ctime=NOW_NS, btime=NOW_NS,
                                   generation=gen)))
        items.extend(build_extent_items(oid, blk, data, gen))
        for i, db in enumerate(build_data_blocks(data)):
            data_blocks.append((blk + i, db))
        file_layouts.append((name, blk, nblocks, len(data), data_csum))
        oid += 1
        blk += nblocks
    items.extend(build_dir_entry_items(L.OBJID_ROOT, dir_entries))
    items.sort(key=lambda it: struct.unpack(L.KEY_FMT, it[0]))

    # split the items across two leaves; the internal root routes between them.
    # Half-and-half keeps both leaves non-empty regardless of how many fixtures
    # are present (this image is deliberately a 2-level-descent regression, so
    # it forces a split rather than relying on the auto-packer -- the files are
    # tiny and would otherwise fit in a single leaf).
    split = len(items) // 2
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
    # (6) + all file data blocks. Block 0 is counted used so the kernel's
    # allocator oracle (which reserves it) agrees with this hint.
    used = 6 + sum(nblocks for (_name, _start, nblocks, _size, _csum) in file_layouts)
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


# ---------------------------------------------------------------------------
# Nested-directory fixture. Proves the formatter builds a multi-LEVEL directory
# tree (not just a flat root) that the kernel can traverse -- the prerequisite
# for the /system + /data layout migration (docs/USERSPACE.md). Kept small and
# known-content so `test dirtree` can assert exact bytes at a nested path.
#
# Object names carry '/'; build_root_items auto-creates every intermediate
# directory. Two entries are EXPLICIT empty directories (DT_DIR) to prove those
# work too -- /data/tmp and /data/users/teo are the real layout's empty dirs.
# ---------------------------------------------------------------------------
DIRTREE_PROBE_PATH    = b"system/bin/hello.txt"
DIRTREE_PROBE_CONTENT = b"hi from /system/bin\n"   # test dirtree asserts these exact bytes

def make_dirtree_image(path: str, size_bytes: int = 1024 * 1024):
    objects = [
        (b"top.txt",                 L.DT_REG, L.S_IFREG | L.PERM_FILE,
         b"root still holds files beside directories\n"),
        (DIRTREE_PROBE_PATH,         L.DT_REG, L.S_IFREG | L.PERM_FILE,
         DIRTREE_PROBE_CONTENT),
        (b"system/lib/note.txt",     L.DT_REG, L.S_IFREG | L.PERM_FILE,
         b"a file two levels down\n"),
        (b"data/apps/foo/foo.txt",   L.DT_REG, L.S_IFREG | L.PERM_FILE,
         b"three levels down\n"),
        (b"data/tmp",                L.DT_DIR, L.S_IFDIR | L.PERM_DIR, None),
        (b"data/users/teo",          L.DT_DIR, L.S_IFDIR | L.PERM_DIR, None),
    ]
    make_image(path, size_bytes, objects=objects)


if __name__ == "__main__":
    import sys
    if len(sys.argv) > 1 and sys.argv[1] == "--dirtree":
        # Nested-directory fixture: mkfs_embkfs.py --dirtree <path>
        make_dirtree_image(sys.argv[2] if len(sys.argv) > 2 else "dirtree.img")
    elif len(sys.argv) > 1 and sys.argv[1] == "--encrypted":
        # Encrypted test fixture: mkfs_embkfs.py --encrypted <path> [passphrase]
        # (v2.2 Phase 4) -- defaults to a fixed test passphrase so automated
        # boot tests can type it via QEMU's monitor without a human present.
        out_path = sys.argv[2] if len(sys.argv) > 2 else "embkfs_encrypted.img"
        passphrase = (sys.argv[3] if len(sys.argv) > 3 else "correcthorsebattery").encode()
        make_encrypted_image(out_path, passphrase)
    elif len(sys.argv) > 1:
        # Single-image mode: write one flat oracle image at the given path
        # (used by the Makefile to produce a second, independent EMBKFS
        # image for multi-volume / USB-mount testing without disturbing the
        # default embkfs.img/embkfs_tree.img pair below).
        make_image(sys.argv[1])
    else:
        make_image("embkfs.img")            # flat: single leaf (collision regression)
        print()
        make_tree_image("embkfs_tree.img")  # tall: internal root + two leaves