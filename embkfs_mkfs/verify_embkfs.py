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

from crc32c import crc32c
import layout as L


def die(msg):
    print("FAIL: " + msg)
    sys.exit(1)


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
        "checksum": stored_csum,
        "fields": fields,
    }


def main(path):
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

    KNOWN_INCOMPAT = 0
    KNOWN_RO_COMPAT = 0
    if fincompat & ~KNOWN_INCOMPAT:
        die(f"unknown incompat features 0x{fincompat:016X} — refuse mount")
    read_only = bool(fro & ~KNOWN_RO_COMPAT)
    print(f"  features compat=0x{fcompat:X} ro_compat=0x{fro:X} incompat=0x{fincompat:X}"
          f"  -> {'READ-ONLY' if read_only else 'read-write'}")

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
            if is_hole:
                if disk_block != 0 or length_blocks != 0 or data_csum != 0:
                    die(f"object {oid}: hole extent has non-zero disk/len/checksum")
                if logical_size == 0:
                    die(f"object {oid}: hole extent has zero logical_size")
                data.extend(b"\x00" * logical_size)
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
    # dir-entry item (same hash 0xC38842AB) and are told apart by name.
    for nm in (b"hello.txt", b"wgyehkb.txt", b"illoeuw.txt"):
        tgt_oid, _ttype = lookup(nm)
        read_object(tgt_oid)

    link_oid, _link_type = lookup(b"hello.lnk")
    read_object(link_oid)

    print("\nALL CHECKS PASSED — image is a valid EMBKFS volume.")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "embkfs.img")