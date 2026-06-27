# EMBKFS Specification — Corrections & Additions (v2.0 → v2.1)

These five items were discovered while implementing and validating the
**read-only mount path**. Each is reflected consistently in the formatter
(`mkfs_embkfs.py`), the verifier (`verify_embkfs.py`), and the kernel C reader,
and each is validated against a known-good oracle image. Fold these into the
canonical specification; section numbers below refer to v2.0.

Principle throughout: the Python formatter/verifier are the ground truth, and
the on-disk layout is whatever they emit — these corrections record where v2.0's
prose diverged from that truth, plus two structures v2.0 left underspecified.

---

## 1. Inode — the reserved field is **48 bytes**, not 40 (§9.1)

The inode item is 128 bytes. Its defined fields total **80 bytes**, so the
trailing reserved area is **48 bytes** (80 + 48 = 128). v2.0's text implied 40,
which does not sum to 128. The size contract
`_Static_assert(sizeof(struct embk_inode_item) == 128)` holds only with
`reserved[48]`.

Authoritative layout (`INODE_FMT = "<QQQ IIII QQQQ Q 48s"`):

| Offset | Size | Field      |
| -----: | ---: | ---------- |
| 0      | 8    | size (logical bytes) |
| 8      | 8    | blocks     |
| 16     | 8    | links      |
| 24     | 4    | mode       |
| 28     | 4    | uid        |
| 32     | 4    | gid        |
| 36     | 4    | flags      |
| 40     | 8    | atime      |
| 48     | 8    | mtime      |
| 56     | 8    | ctime      |
| 64     | 8    | btime      |
| 72     | 8    | generation |
| 80     | 48   | **reserved** |

---

## 2. Directory-entry name hash = CRC32C of the name, in the key's offset (§7, §9.2)

A directory entry is keyed `{ object_id = directory, type = DIR_ENTRY (16),
offset = name_hash }`. The **name_hash is the CRC32C of the entry's name bytes**,
stored in the low 32 bits of the key's 64-bit `offset` field (high 32 bits
zero). This is what lets a name be located by key without scanning the
directory. Because the hash is only 32 bits, a hash match is **not** proof of a
name match — see correction 3.

---

## 3. Collision handling — chained directory-entry records (§9.2)

Two distinct names can share a name_hash (a real same-length collision was
found and is now a permanent regression fixture). The format handles this by
**chaining**, not by probing other keys:

- All directory entries that share one name_hash live in **one** dir-entry item
  (one key `{dir, DIR_ENTRY, hash}`).
- That item's data is a sequence of dir-entry records packed back-to-back. Each
  record is the 16-byte fixed part followed by `name_len` UTF-8 name bytes:

  `DIR_ENTRY_FIXED_FMT = "<QBB6s"` →

  | Offset | Size | Field |
  | -----: | ---: | ----- |
  | 0      | 8    | target_object_id |
  | 8      | 1    | target_type |
  | 9      | 1    | name_len |
  | 10     | 6    | reserved |
  | 16     | name_len | name (UTF-8, not NUL-terminated) |

- **No record count is stored.** The chain is bounded solely by the item's
  `size`; a reader walks records (`record_len = 16 + name_len`) until the item's
  bytes are exhausted. The item size is the single source of truth.
- Resolution is **authoritative by name**: walk the chain and compare the actual
  name bytes (length first, then `memcmp`); the matching record's
  `target_object_id` is the answer. The hash only selects the item; the name
  selects the record. A reader that trusts the hash alone will silently return
  the wrong file on a collision.

The common (non-colliding) case is just a one-record chain — the same code path.

---

## 4. Internal node layout — contiguous {key, ptr} slots (§8.2)

v2.0 named the internal-node slot but did not pin its on-disk arrangement. An
internal node (node header `level > 0`) is the standard 40-byte node header
followed by a **contiguous array of fixed-size slots** — no slotted-page
indirection (that is the leaf's mechanism, needed only for variable-size item
data).

- Slot layout: `key (24) || block_ptr (32)` = **56 bytes**, repeated.
- Slots begin immediately after the node header (offset 40) and are stored in
  **ascending key order**.
- `nritems` in the node header is the **slot count**.
- **Each slot's key is the smallest key present anywhere in that child's
  subtree.** This is the routing invariant: a key `K` belongs under slot `i`
  iff `slot[i].key <= K < slot[i+1].key`.
- By convention the formatter sets slot 0's key to the true minimum key of the
  whole subtree, so no valid target can sort below the first slot.

Descent rule (read path): choose the **rightmost slot whose key is `<=` the
target** (an upper-bound search), follow its `block_ptr`, and recurse. The `<=`
is load-bearing: a target that exactly equals `slot[i+1].key` must descend into
child `i+1` (that key is the *smallest* key of subtree `i+1`), not child `i`.
Every followed pointer is verified by the normal node check, so the Merkle chain
extends to full tree depth.

---

## 5. Key ordering is **field-wise**, not byte-wise (§7)

Keys are compared as the integer tuple **(object_id, type, offset)**, most-
significant field first:

```
cmp(a, b):  compare a.object_id vs b.object_id  (as 64-bit integers)
            then a.type vs b.type
            then a.offset vs b.offset
```

A key is **not** compared as a raw little-endian byte string. `KEY_FMT` is
`"<QQQ"` (little-endian), so a byte-wise `memcmp` orders the low-order bytes
first and produces a different, wrong order whenever the distinguishing field is
large (e.g. object_id 1 vs 256 compares as `01..` > `00..` under memcmp). This
field-wise rule is the single ordering authority across all three components: it
governs the strictly-increasing-key invariant within a leaf, the slot order
within an internal node, and the descent comparison. (This corrected a real bug
in the formatter's item sort and the verifier's ordering check; the kernel's
`embk_key_cmp` was field-wise from the start.)

---

### Validation status

All five are exercised by the oracle images and boot green in the kernel:

- a **flat image** (single leaf) — collision regression (corrections 2, 3, 5);
- a **2-level tree image** whose split lands a key exactly on a slot boundary —
  descent and the `<=` boundary (corrections 4, 5);

with every node Merkle-verified against its parent and every file's data
checksum verified over its logical size.
