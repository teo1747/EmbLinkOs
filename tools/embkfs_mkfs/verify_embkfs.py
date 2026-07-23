#!/usr/bin/env python3
"""
verify_embkfs.py — read an EMBKFS image back and validate it end-to-end.

This is the formatter's own oracle: if this passes, the image is internally
consistent. It also doubles as a REFERENCE for the EmbLink kernel's read path —
every check here is a check the kernel must perform. Reading this top to bottom
is essentially the read-side algorithm in miniature.

It performs, in order:
    1. Mount: read + validate the superblock (magic, checksum, version/features)
    2. Walk the WHOLE tree from the root pointer, verifying every node
       (magic, self-checksum, Merkle link to its parent pointer, generation,
       self-block) and the B-tree routing invariant at each internal node;
       collect every leaf's items.
    3. Resolve files (incl. a hash-collision chain): root dir inode -> dir entry
       by name hash -> file inode -> extent -> read + verify the data.

Works at any tree depth: a single-leaf (flat) image is just a zero-internal-node
walk. Mixed generations across the tree are expected after a copy-on-write commit
(an untouched sibling keeps its old generation), and are validated correctly
because every node's generation is checked against ITS parent pointer, not
against any single global value.
"""

import sys
import struct
import time

from crc32c import crc32c


def embk_decompress(comp: bytes, expected_len: int) -> bytes:
    """Python port of kernel/fs/embkfs/embkfs_compress.c's embk_decompress().

    LZ4-inspired token stream with NO end-of-block marker: the caller
    already knows the exact decompressed length (the extent's logical_size),
    so decoding just stops once that many bytes have been produced. Must
    stay byte-for-byte in sync with the C decoder -- this IS the independent
    oracle for it (v2.2 Phase 3)."""
    out = bytearray()
    sp = 0
    n = len(comp)
    while len(out) < expected_len:
        if sp >= n:
            raise ValueError("embk_decompress: truncated stream (token)")
        token = comp[sp]; sp += 1
        lit_len = token >> 4
        match_nib = token & 0x0F
        if lit_len == 15:
            while True:
                if sp >= n:
                    raise ValueError("embk_decompress: truncated stream (literal ext)")
                b = comp[sp]; sp += 1
                lit_len += b
                if b != 255:
                    break
        if sp + lit_len > n:
            raise ValueError("embk_decompress: truncated stream (literals)")
        out.extend(comp[sp:sp + lit_len])
        sp += lit_len
        if len(out) == expected_len:
            break
        if sp + 2 > n:
            raise ValueError("embk_decompress: truncated stream (offset)")
        offset = comp[sp] | (comp[sp + 1] << 8)
        sp += 2
        if offset == 0 or offset > len(out):
            raise ValueError(f"embk_decompress: bad match offset {offset}")
        match_len = match_nib
        if match_nib == 15:
            while True:
                if sp >= n:
                    raise ValueError("embk_decompress: truncated stream (match ext)")
                b = comp[sp]; sp += 1
                match_len += b
                if b != 255:
                    break
        match_len += 4  # MINMATCH, must match EMBKZ_MINMATCH in the C encoder
        msrc = len(out) - offset
        for i in range(match_len):
            out.append(out[msrc + i])
    if len(out) != expected_len:
        raise ValueError("embk_decompress: decompressed length mismatch")
    return bytes(out)
import layout as L


def xts_decrypt_blocks(data_key: bytes, tweak_key: bytes, ciphertext: bytes, disk_block_start: int) -> bytes:
    """Decrypts a run of on-disk filesystem blocks, each its own independent
    XTS "sector" tweaked by its own disk block number -- must match
    kernel/crypto/xts.c's block_number_to_tweak_input() + aes_xts_decrypt()
    exactly. Uses Python's `cryptography` package, NOT a port of the
    kernel's own AES/XTS code, so a bug shared by both can't hide from this
    independent oracle (v2.2 Phase 4)."""
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
    assert len(ciphertext) % L.BLOCK_SIZE == 0
    out = bytearray()
    n = len(ciphertext) // L.BLOCK_SIZE
    for i in range(n):
        block_num = disk_block_start + i
        tweak = block_num.to_bytes(8, "little") + b"\x00" * 8
        c = Cipher(algorithms.AES(data_key + tweak_key), modes.XTS(tweak))
        d = c.decryptor()
        chunk = ciphertext[i * L.BLOCK_SIZE:(i + 1) * L.BLOCK_SIZE]
        out.extend(d.update(chunk) + d.finalize())
    return bytes(out)


def die(msg):
    print("FAIL: " + msg)
    sys.exit(1)


def check_inode_timestamps(label, inode_data):
    """v2.2: every object mkfs_embkfs.py writes is stamped with a real
    wall-clock 'now' at format time (all four fields equal, since nothing
    has been modified/accessed since). Sanity-checks that: non-zero,
    atime==btime==mtime==ctime, and not implausibly in the future (a
    generous 1-day margin above the CURRENT wall clock, not the format
    time, to tolerate this verifier running some time after the image was
    formatted)."""
    (_size, _blocks, _links, _mode, _uid, _gid, _flags,
     atime, mtime, ctime, btime, _generation, _reserved) = struct.unpack(L.INODE_FMT, inode_data)

    if atime == 0 or mtime == 0 or ctime == 0 or btime == 0:
        die(f"{label}: zero timestamp (a={atime} m={mtime} c={ctime} b={btime})")
    if not (atime == mtime == ctime == btime):
        die(f"{label}: freshly-formatted timestamps not all equal (a={atime} m={mtime} c={ctime} b={btime})")

    now_ns = int(time.time() * 1_000_000_000)
    one_day_ns = 24 * 60 * 60 * 1_000_000_000
    if btime > now_ns + one_day_ns:
        die(f"{label}: btime {btime} is implausibly in the future (now={now_ns})")

    print(f"    -> timestamps OK ({label}: btime=mtime=ctime=atime={btime})")


def read_block(img, block_no):
    off = block_no * L.BLOCK_SIZE
    return img[off:off + L.BLOCK_SIZE]


def read_blocks(img, block_no, nblocks):
    off = block_no * L.BLOCK_SIZE
    return img[off:off + nblocks * L.BLOCK_SIZE]


def walk_tree(img, blk, csum, gen, items, depth=0):
    """Verify the node at (blk, csum, gen) and every node beneath it, appending
    each leaf's items to `items`. Returns the SMALLEST key (as an integer tuple)
    present in this subtree, so the caller can check the routing invariant.

    Verifies at every node: node magic, self-checksum over [8..end], the Merkle
    link (self-checksum == the parent pointer's checksum), generation == the
    parent pointer's generation, and self-block-number == the pointer target.
    """
    indent = "  " + "  " * depth
    node = read_block(img, blk)

    (n_csum, n_magic, n_gen, n_block, n_level, _resv, n_nritems) = \
        struct.unpack_from(L.NODE_HDR_FMT, node, 0)

    if n_magic != L.EMBKFS_NODE_MAGIC:
        die(f"block {blk}: bad node magic 0x{n_magic:016X} (expected EMBKNODE)")
    calc = crc32c(node[8:])
    if n_csum != calc:
        die(f"block {blk}: self-checksum 0x{n_csum:08X} != calc 0x{calc:08X}")
    if n_csum != csum:
        die(f"block {blk}: checksum 0x{n_csum:08X} != parent pointer 0x{csum:08X} (Merkle break)")
    if n_gen != gen:
        die(f"block {blk}: generation {n_gen} != pointer generation {gen} (stale/reused block?)")
    if n_block != blk:
        die(f"block {blk}: self-block {n_block} != pointer target {blk} (misplaced block?)")

    kind = "LEAF" if n_level == 0 else "internal"
    print(f"{indent}block {blk:>3}: {kind:<8} level {n_level}  nritems {n_nritems}  "
          f"csum 0x{n_csum:08X}  gen {n_gen}   [verified vs parent]")

    if n_level == 0:
        # ---- LEAF: collect items, enforce strictly-increasing key order ----
        prev_key = None
        subtree_min = None
        for i in range(n_nritems):
            hpos = L.NODE_HDR_SIZE + i * L.ITEM_HDR_SIZE
            key_blob, d_off, d_size = struct.unpack_from(L.ITEM_HDR_FMT, node, hpos)
            oid, typ, koff = struct.unpack(L.KEY_FMT, key_blob)
            key_tuple = (oid, typ, koff)            # FIELD-WISE, not raw LE bytes
            if prev_key is not None and key_tuple <= prev_key:
                die(f"block {blk}: items not sorted by key at index {i}")
            prev_key = key_tuple
            if subtree_min is None:
                subtree_min = key_tuple
            items.append((oid, typ, koff, node[d_off:d_off + d_size]))
        if subtree_min is None:
            die(f"block {blk}: empty leaf")
        return subtree_min

    # ---- INTERNAL: ordered {key, ptr} slots; recurse + check routing ----
    if n_nritems == 0:
        die(f"block {blk}: empty internal node")
    SLOT = L.KEY_SIZE + L.BLOCK_PTR_SIZE
    prev_slot_key = None
    subtree_min = None
    for i in range(n_nritems):
        spos = L.NODE_HDR_SIZE + i * SLOT
        slot_key = struct.unpack(L.KEY_FMT, node[spos:spos + L.KEY_SIZE])
        c_block, c_csum, c_gen, _c_flags = struct.unpack(
            L.BLOCK_PTR_FMT, node[spos + L.KEY_SIZE:spos + SLOT])

        if prev_slot_key is not None and slot_key <= prev_slot_key:
            die(f"block {blk}: internal slots not sorted at index {i}")
        prev_slot_key = slot_key

        # recurse: verify the whole child subtree, get its smallest key
        child_min = walk_tree(img, c_block, c_csum, c_gen, items, depth + 1)

        # ROUTING INVARIANT: a slot's key must equal the smallest key in the
        # subtree it points at — this is exactly what makes descent correct
        # (the rightmost slot with key <= target leads to the right leaf).
        if child_min != slot_key:
            die(f"block {blk}: slot {i} key {slot_key} != child subtree min {child_min} "
                f"(routing invariant broken)")

        if subtree_min is None:
            subtree_min = child_min     # slot[0].key == smallest key overall
    return subtree_min


def parse_superblock_candidate(raw512):
    if len(raw512) < L.SUPERBLOCK_SIZE:
        return None
    sb = raw512[:L.SUPERBLOCK_SIZE]
    magic = struct.unpack_from("<Q", sb, 0)[0]
    if magic != L.EMBKFS_MAGIC:
        return None
    stored_csum = struct.unpack_from("<Q", sb, L.SB_BODY_SIZE)[0]
    calc_csum = crc32c(sb[:L.SB_BODY_SIZE])
    if stored_csum != calc_csum:
        return None
    fields = struct.unpack(L.SB_BODY_FMT, sb[:L.SB_BODY_SIZE])
    return {
        "raw": sb,
        "raw512": raw512,   # full sector -- the crypto header (v2.2 Phase 4)
                            # lives past SB_BODY_SIZE, outside `sb`/`raw`
        "checksum": stored_csum,
        "fields": fields,
    }


def main(path, passphrase=None):
    with open(path, "rb") as f:
        img = f.read()
    total_blocks = len(img) // L.BLOCK_SIZE
    print(f"Opened {path}: {len(img)} bytes, {total_blocks} blocks\n")

    # ---------------------------------------------------------------
    # 1. MOUNT — validate primary + backup superblocks and choose newest valid
    # ---------------------------------------------------------------
    print("== 1. Superblock ==")

    p0 = L.SUPERBLOCK_OFFSET
    primary_raw = img[p0:p0 + 512]
    primary = parse_superblock_candidate(primary_raw)

    backup = None
    for bs in (4096, 8192, 16384, 32768, 65536):
        if len(img) < bs:
            continue
        b0 = len(img) - bs
        cand = parse_superblock_candidate(img[b0:b0 + 512])
        if not cand:
            continue
        f = cand["fields"]
        if f[6] == bs:  # block_size field
            backup = cand
            break

    if not primary and not backup:
        die("neither primary nor backup superblock is valid")

    if primary and backup:
        pgen = primary["fields"][10]
        bgen = backup["fields"][10]
        chosen = backup if bgen > pgen else primary
        if chosen is backup:
            print(f"  using newer backup superblock (gen {bgen} > {pgen})")
    else:
        chosen = primary if primary else backup
        if chosen is backup:
            print("  primary invalid; using backup superblock")

    sb = chosen["raw"]
    (_magic, vmaj, vmin, fcompat, fro, fincompat,
     block_size, tot_blocks, free_blocks, uuid16, generation,
     root_ptr, checkpoint_ptr) = chosen["fields"]

    print(f"  magic OK (0x{_magic:016X} = \"EMBKFS17\")")
    print(f"  checksum OK (0x{chosen['checksum']:08X})")

    print(f"  version {vmaj}.{vmin}, block_size {block_size}, total_blocks {tot_blocks}, "
          f"free {free_blocks}, gen {generation}")

    KNOWN_INCOMPAT = (L.EMBKFS_INCOMPAT_COMPRESSION | L.EMBKFS_INCOMPAT_ENCRYPTED |
                      L.EMBKFS_INCOMPAT_VERIFIED_ROOT | L.EMBKFS_INCOMPAT_SNAPREG)
    KNOWN_RO_COMPAT = 0
    if fincompat & ~KNOWN_INCOMPAT:
        die(f"unknown incompat features 0x{fincompat:016X} — refuse mount")
    read_only = bool(fro & ~KNOWN_RO_COMPAT)
    print(f"  features compat=0x{fcompat:X} ro_compat=0x{fro:X} incompat=0x{fincompat:X}"
          f"  -> {'READ-ONLY' if read_only else 'read-write'}")

    # ---------------------------------------------------------------
    # 1a. SNAPSHOT REGISTRY (v2.3) -- the block that makes rollback reversible.
    #
    # Checked HERE, before the tree walk, because §2's allocator oracle depends
    # on it: the kernel reserves this block, so if the image did not account
    # for it the free-block count would be off by one and the mismatch would
    # surface as a confusing arithmetic error instead of a missing feature.
    #
    # Independent of the kernel on purpose (the point of this verifier): the
    # checksum is recomputed here from the raw bytes rather than trusted.
    # ---------------------------------------------------------------
    snapreg_entries = []
    if fincompat & L.EMBKFS_INCOMPAT_SNAPREG:
        print("\n== 1a. Snapshot registry (v2.3, out of the CoW tree) ==")
        reg = img[L.SNAPREG_BLOCK * block_size:(L.SNAPREG_BLOCK + 1) * block_size]
        body = reg[:L.SNAPREG_BODY_SIZE]
        csum, magic, count, reserved = struct.unpack(L.SNAPREG_HDR_FMT, body[:L.SNAPREG_HDR_SIZE])
        if magic != L.SNAPREG_MAGIC:
            die(f"INCOMPAT_SNAPREG set but registry magic at block {L.SNAPREG_BLOCK} "
                f"is 0x{magic:016X}, want 0x{L.SNAPREG_MAGIC:016X}")
        want = crc32c(body[8:])
        if (csum & 0xFFFFFFFF) != want:
            die(f"snapshot registry checksum 0x{csum:08X} != computed 0x{want:08X}")
        if reserved != 0:
            die(f"snapshot registry reserved field is {reserved}, must be 0")
        if count > L.MAX_SNAPSHOTS:
            die(f"snapshot registry count {count} exceeds MAX_SNAPSHOTS {L.MAX_SNAPSHOTS}")
        print(f"  block {L.SNAPREG_BLOCK}: magic OK, checksum OK (0x{want:08X}), "
              f"{count}/{L.MAX_SNAPSHOTS} slot(s) in use")
        for i in range(count):
            off = L.SNAPREG_HDR_SIZE + i * L.SNAPSHOT_ITEM_SIZE
            ent = body[off:off + L.SNAPSHOT_ITEM_SIZE]
            name = ent[:32].split(b"\x00")[0].decode("ascii", "replace")
            root_blk, root_csum = struct.unpack_from("<QQ", ent, 32)[0], None
            gen_at, ts = struct.unpack_from("<QQ", ent, 64)
            if root_blk == 0 or root_blk >= tot_blocks:
                die(f"snapshot '{name}' root block {root_blk} is out of range")
            print(f"    [{i}] '{name}' -> root block {root_blk}, gen {gen_at}")
            snapreg_entries.append((name, root_blk))
        if count == 0:
            print("    (empty -- a freshly formatted volume)")

    # ---------------------------------------------------------------
    # 1b. ENCRYPTION (v2.2 Phase 4) -- independent Python-side unlock,
    # deliberately using the `cryptography` package's AES-XTS rather than
    # reusing the kernel's own kernel/crypto/xts.c, so a bug shared by both
    # implementations can't hide from this oracle.
    # ---------------------------------------------------------------
    xts_data_key = xts_tweak_key = None
    if fincompat & L.EMBKFS_INCOMPAT_ENCRYPTED:
        if not passphrase:
            die("volume is encrypted -- pass --passphrase to verify it")
        raw512 = chosen["raw512"]
        hdr_bytes = raw512[L.CRYPTO_HEADER_OFFSET:L.CRYPTO_HEADER_OFFSET + L.CRYPTO_HEADER_SIZE]
        magic, salt, iterations, check, _reserved = struct.unpack(L.CRYPTO_HEADER_FMT, hdr_bytes)
        if magic != L.EMBKFS_CRYPTO_HEADER_MAGIC:
            die(f"ENCRYPTED feature set but crypto header magic is wrong (0x{magic:016X})")
        import hashlib
        keymat = hashlib.pbkdf2_hmac("sha256", passphrase.encode(), salt, iterations, 64)
        xts_data_key, xts_tweak_key = keymat[:32], keymat[32:]
        from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
        c = Cipher(algorithms.AES(xts_data_key + xts_tweak_key), modes.XTS(b"\x00" * 16))
        e = c.encryptor()
        got_check = e.update(L.KEY_CHECK_PLAINTEXT) + e.finalize()
        if got_check != check:
            die("wrong passphrase (key-check ciphertext mismatch)")
        print(f"  encrypted volume unlocked (kdf_iterations={iterations})")

    if block_size != L.BLOCK_SIZE:
        die(f"this verifier assumes block_size {L.BLOCK_SIZE}, image has {block_size}")

    root_block, root_csum, root_gen, root_flags = struct.unpack(L.BLOCK_PTR_FMT, root_ptr)
    print(f"  root ptr -> block {root_block} (csum 0x{root_csum:08X}, gen {root_gen})")

    # ---------------------------------------------------------------
    # 2. TREE — descend from the root, verifying every node + the routing
    #    invariant, collecting every leaf's items
    # ---------------------------------------------------------------
    print("\n== 2. Tree (verify every node vs its parent; collect all items) ==")
    items = []   # (object_id, type, offset, data_bytes) across ALL leaves
    tree_min = walk_tree(img, root_block, root_csum, root_gen, items)
    print(f"  tree verified end-to-end: {len(items)} items across all leaves; "
          f"smallest key {tree_min}")

    def find_item(object_id, type_, offset=None):
        for (oid, typ, off, data) in items:
            if oid == object_id and typ == type_ and (offset is None or off == offset):
                return (oid, typ, off, data)
        return None

    def find_items(object_id, type_):
        out = []
        for (oid, typ, off, data) in items:
            if oid == object_id and typ == type_:
                out.append((oid, typ, off, data))
        out.sort(key=lambda it: it[2])
        return out

    # ---------------------------------------------------------------
    # 3. RESOLVE directory entries (incl. a collision chain) + read files
    # ---------------------------------------------------------------
    print("\n== 3. Resolve files ==")

    root_dir = find_item(L.OBJID_ROOT, L.EMBK_TYPE_INODE, 0)
    if not root_dir:
        die("root directory inode not found")
    (_, _, _, rd_data) = root_dir
    (rd_size, rd_blocks, rd_links, rd_mode, *_rest) = struct.unpack(L.INODE_FMT, rd_data)
    is_dir = (rd_mode & 0o170000) == L.S_IFDIR
    print(f"  root inode: mode 0o{rd_mode:06o} ({'dir' if is_dir else 'not dir'}), links {rd_links}")
    if not is_dir:
        die("root object is not a directory")
    check_inode_timestamps("root", rd_data)

    def lookup(name):
        """Resolve one name in the root dir to its target object id, WALKING the
        dir-entry chain. The key offset is the name hash; that one item may hold
        several records (a collision chain) packed back-to-back and bounded by
        its size — we name-compare each until we find the requested one."""
        nh = crc32c(name) & 0xFFFFFFFF
        de = find_item(L.OBJID_ROOT, L.EMBK_TYPE_DIR_ENTRY, nh)
        if not de:
            die(f"directory entry for {name!r} (hash 0x{nh:08X}) not found")
        (_, _, _, de_data) = de

        records = []                       # (name, target_oid)
        off = 0
        while off + L.DIR_ENTRY_FIXED_SIZE <= len(de_data):
            tgt_oid, _typ, name_len, _resv = struct.unpack_from(L.DIR_ENTRY_FIXED_FMT, de_data, off)
            end = off + L.DIR_ENTRY_FIXED_SIZE + name_len
            if end > len(de_data):
                die(f"chain for hash 0x{nh:08X}: a record name runs past the item")
            records.append((de_data[off + L.DIR_ENTRY_FIXED_SIZE:end], tgt_oid))
            off = end
        if off != len(de_data):
            die(f"chain for hash 0x{nh:08X}: trailing bytes after last record")

        chain = [n.decode() for n, _ in records]
        suffix = f"  (collision chain: {chain})" if len(records) > 1 else ""
        for rec_name, tgt_oid in records:
            if rec_name == name:            # authoritative: the NAME, not the hash
                print(f"  {name.decode():<13} hash 0x{nh:08X} -> object {tgt_oid}{suffix}")
                return tgt_oid, _typ
        die(f"name {name!r} not in chain for hash 0x{nh:08X} (held {chain})")

    def read_object_bytes(oid, mode):
        ext_items = find_items(oid, L.EMBK_TYPE_EXTENT)
        if not ext_items:
            return b""

        data = bytearray()
        expect_off = 0
        for (_eo, _et, off, ext_data) in ext_items:
            if off != expect_off:
                die(f"object {oid}: non-contiguous extent map at off {off}, expected {expect_off}")

            (disk_block, length_blocks, logical_size, data_csum,
             _egen, flags, _r0, _r1) = struct.unpack(L.EXTENT_FMT, ext_data)

            is_hole = (flags & L.EXTENT_FLAG_HOLE) != 0
            is_compressed = (flags & L.EXTENT_FLAG_COMPRESSED) != 0
            is_encrypted = (flags & L.EXTENT_FLAG_ENCRYPTED) != 0
            if is_hole:
                if disk_block != 0 or length_blocks != 0 or data_csum != 0:
                    die(f"object {oid}: hole extent has non-zero disk/len/checksum")
                if logical_size == 0:
                    die(f"object {oid}: hole extent has zero logical_size")
                data.extend(b"\x00" * logical_size)
            elif is_encrypted:
                if length_blocks == 0:
                    die(f"object {oid}: non-hole extent has zero length")
                if not xts_data_key:
                    die(f"object {oid}: extent@{off} is ENCRYPTED but no passphrase was given")
                cap = length_blocks * L.BLOCK_SIZE
                # Checksum covers the WHOLE on-disk run (ciphertext, padding
                # included) -- matches the kernel's write-time order
                # (encrypt, then checksum) exactly (v2.2 Phase 4).
                raw = read_blocks(img, disk_block, length_blocks)
                calc = crc32c(raw)
                if calc != data_csum:
                    die(f"object {oid}: extent@{off} checksum 0x{calc:08X} != stored 0x{data_csum:08X} "
                        f"(ciphertext)")
                plain = xts_decrypt_blocks(xts_data_key, xts_tweak_key, raw, disk_block)
                if is_compressed:
                    comp_size = struct.unpack_from("<Q", _r1, 0)[0]
                    if comp_size == 0 or comp_size > cap:
                        die(f"object {oid}: compressed_size {comp_size} invalid for length {length_blocks}")
                    ext_bytes = embk_decompress(plain[:comp_size], logical_size)
                else:
                    if logical_size == 0 or logical_size > cap:
                        die(f"object {oid}: logical_size {logical_size} invalid for length {length_blocks}")
                    ext_bytes = plain[:logical_size]
                data.extend(ext_bytes)
            elif is_compressed:
                if length_blocks == 0:
                    die(f"object {oid}: non-hole extent has zero length")
                comp_size = struct.unpack_from("<Q", _r1, 0)[0]
                cap = length_blocks * L.BLOCK_SIZE
                if comp_size == 0 or comp_size > cap:
                    die(f"object {oid}: compressed_size {comp_size} invalid for length {length_blocks}")
                if logical_size == 0:
                    die(f"object {oid}: compressed extent has zero logical_size")
                comp_bytes = read_blocks(img, disk_block, length_blocks)[:comp_size]
                calc = crc32c(comp_bytes)
                if calc != data_csum:
                    die(f"object {oid}: extent@{off} checksum 0x{calc:08X} != stored 0x{data_csum:08X} "
                        f"(compressed bytes)")
                ext_bytes = embk_decompress(comp_bytes, logical_size)
                data.extend(ext_bytes)
            else:
                if length_blocks == 0:
                    die(f"object {oid}: non-hole extent has zero length")
                cap = length_blocks * L.BLOCK_SIZE
                if logical_size == 0 or logical_size > cap:
                    die(f"object {oid}: logical_size {logical_size} invalid for length {length_blocks}")
                ext_bytes = read_blocks(img, disk_block, length_blocks)[:logical_size]
                calc = crc32c(ext_bytes)
                if calc != data_csum:
                    die(f"object {oid}: extent@{off} checksum 0x{calc:08X} != stored 0x{data_csum:08X}")
                data.extend(ext_bytes)

            expect_off += logical_size

        return bytes(data)

    def read_object(oid):
        """Read + verify regular files and symlinks from inode + extent map."""
        fi = find_item(oid, L.EMBK_TYPE_INODE, 0)
        if not fi:
            die(f"file inode for object {oid} not found")
        (_, _, _, fi_data) = fi
        (f_size, f_blocks, f_links, f_mode, *_r) = struct.unpack(L.INODE_FMT, fi_data)
        mode = f_mode & L.S_IFMT
        if mode not in (L.S_IFREG, L.S_IFLNK):
            die(f"object {oid} is neither regular file nor symlink")
        check_inode_timestamps(f"object {oid}", fi_data)

        obj_bytes = read_object_bytes(oid, mode)
        if len(obj_bytes) != f_size:
            die(f"object {oid}: extent bytes {len(obj_bytes)} != inode size {f_size}")

        if mode == L.S_IFLNK:
            text = obj_bytes.decode("utf-8", errors="replace")
            print(f"    -> symlink size {f_size}: target {text!r}")
        else:
            text = obj_bytes.decode("utf-8", errors="replace").rstrip()
            print(f"    -> file size {f_size}, {len(find_items(oid, L.EMBK_TYPE_EXTENT))} extent(s) OK: {text!r}")

    # hello.txt is a single-record entry; wgyehkb.txt and illoeuw.txt share one
    # dir-entry item (same hash 0xC38842AB) and are told apart by name. These
    # are the default make_image()/make_tree_image() fixture's own filenames
    # -- other generators (e.g. make_encrypted_image()'s "secret.txt") won't
    # have them, so only resolve whichever of this fixed set actually exists
    # rather than assuming any specific image's contents.
    def name_present(nm):
        nh = crc32c(nm) & 0xFFFFFFFF
        return find_item(L.OBJID_ROOT, L.EMBK_TYPE_DIR_ENTRY, nh) is not None

    for nm in (b"hello.txt", b"wgyehkb.txt", b"illoeuw.txt", b"secret.txt"):
        if not name_present(nm):
            continue
        tgt_oid, _ttype = lookup(nm)
        read_object(tgt_oid)

    if name_present(b"hello.lnk"):
        link_oid, _link_type = lookup(b"hello.lnk")
        read_object(link_oid)

    # ---------------------------------------------------------------
    # 4. WALK THE WHOLE DIRECTORY TREE (recursive)
    # ---------------------------------------------------------------
    # §3 resolves names in the ROOT only. That could not vet a NESTED image
    # (docs/USERSPACE.md's /system + /data), so this descends the entire tree and
    # verifies the structural invariants a flat check never touched:
    #   - every directory inode is actually S_IFDIR;
    #   - every entry resolves to an EXISTING inode whose type MATCHES the entry's
    #     dtype (DT_DIR<->S_IFDIR, DT_REG<->S_IFREG, DT_LNK<->S_IFLNK);
    #   - no cycles (a dir reached twice);
    #   - no orphans (every inode is reachable from root -- a file with no path
    #     is corruption the root-only check could never surface).
    # Structure only, deliberately: content/extent/checksum verification is §3's
    # job and stays there (it handles encryption); the tree walk must run on every
    # image, encrypted or not.
    print("\n== 4. Walk the directory tree ==")

    DTYPE_MODE = {L.DT_DIR: L.S_IFDIR, L.DT_REG: L.S_IFREG, L.DT_LNK: L.S_IFLNK}

    def decode_dir(dir_oid):
        """Every (name, target_oid, dtype) in dir_oid, walking each collision
        chain -- the generalization of lookup() to a full listing."""
        out = []
        for (_o, _t, _off, de_data) in find_items(dir_oid, L.EMBK_TYPE_DIR_ENTRY):
            off = 0
            while off + L.DIR_ENTRY_FIXED_SIZE <= len(de_data):
                tgt, ttype, nlen, _r = struct.unpack_from(L.DIR_ENTRY_FIXED_FMT, de_data, off)
                end = off + L.DIR_ENTRY_FIXED_SIZE + nlen
                if end > len(de_data):
                    die(f"dir {dir_oid}: a record name runs past its item")
                out.append((de_data[off + L.DIR_ENTRY_FIXED_SIZE:end], tgt, ttype))
                off = end
            if off != len(de_data):
                die(f"dir {dir_oid}: trailing bytes after last dir-entry record")
        return out

    def inode_mode_of(oid):
        fi = find_item(oid, L.EMBK_TYPE_INODE, 0)
        if not fi:
            die(f"dir entry points at object {oid}, which has no inode (dangling)")
        return struct.unpack(L.INODE_FMT, fi[3])[3]

    reached = {L.OBJID_ROOT}
    counts = {"dir": 1, "file": 0, "link": 0}   # root pre-counted

    def walk(dir_oid, path, stack):
        if dir_oid in stack:
            die(f"directory cycle: object {dir_oid} reached twice (at {path or '/'})")
        stack = stack | {dir_oid}
        for name, toid, ttype in sorted(decode_dir(dir_oid)):
            childpath = f"{path}/{name.decode(errors='replace')}"
            mode = inode_mode_of(toid) & L.S_IFMT
            want = DTYPE_MODE.get(ttype)
            if want is None:
                die(f"{childpath}: dir entry has unknown dtype {ttype}")
            if mode != want:
                die(f"{childpath}: entry dtype {ttype} but inode mode is 0o{mode:o}")
            reached.add(toid)
            if ttype == L.DT_DIR:
                counts["dir"] += 1
                print(f"  DIR  {childpath}/  (object {toid})")
                walk(toid, childpath, stack)
            elif ttype == L.DT_LNK:
                counts["link"] += 1
                print(f"  LNK  {childpath}  (object {toid})")
            else:
                counts["file"] += 1
                print(f"  file {childpath}  (object {toid})")

    walk(L.OBJID_ROOT, "", set())

    all_inodes = {oid for (oid, typ, _off, _d) in items if typ == L.EMBK_TYPE_INODE}
    orphans = all_inodes - reached
    if orphans:
        die(f"unreachable inode(s) with no directory entry: {sorted(orphans)}")
    print(f"  tree OK: {counts['dir']} dir(s), {counts['file']} file(s), "
          f"{counts['link']} symlink(s); all {len(all_inodes)} inodes reachable")

    print("\nALL CHECKS PASSED — image is a valid EMBKFS volume.")


if __name__ == "__main__":
    args = sys.argv[1:]
    passphrase = None
    if "--passphrase" in args:
        i = args.index("--passphrase")
        passphrase = args[i + 1]
        del args[i:i + 2]
    main(args[0] if args else "embkfs.img", passphrase=passphrase)