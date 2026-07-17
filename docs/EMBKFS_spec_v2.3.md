# EMBKFS Specification — Additions (v2.2 → v2.3)

This document layers v2.3 on top of v2.0 (`EMBKFS_Specification_v2.0.pdf`),
v2.1's corrections (`EMBKFS_spec_v2.1.md`) and v2.2's feature set
(`EMBKFS_spec_v2.2.md`), following the same discipline: everything here is
exactly what the formatter (`mkfs_embkfs.py`), the independent verifier
(`verify_embkfs.py`) and the kernel reader/writer (`kernel/fs/embkfs/embkfs.c`)
actually implement, cross-checked against each other.

**v2.3 changes NO on-disk structure.** There is no new item type, no new field,
no new `feature_incompat` bit, and `version_major` stays **1**. A v2.2 reader
reads a v2.3 image and vice-versa. What changed is *semantics* (one genuinely
new atomic operation), *formatter policy* (extent sizing), and the *read path*
(which got ~140× faster). Those are worth specifying precisely because they are
what a second implementation would otherwise get subtly wrong.

Everything below was driven by real software: **git** forced the atomic rename,
**CPython** forced the read path.

---

## 1. Atomic `rename` — the one real semantic addition

`embkfs_rename()` / `embkfs_rename_path()`. Renaming over an existing name
**replaces it atomically**: the destination's old object is torn down in the
**same B-tree commit** that repoints the name. There is no instant at which the
destination is missing, and no instant at which it names both objects.

This is not gold-plating. **git's lockfile protocol depends on it**: it writes
`X.lock`, then renames it over `X`, and correctness rests on that swap being
indivisible. `git commit` does it for every ref update.

### 1.1 Why `remove` + `add` does NOT compose

The subtle part, and the reason this needs specifying. Directory entries are
keyed by name hash — `{dir_oid, DIR_ENTRY, hash(name)}` — and names that collide
on the hash **share one item** as a chain of records (v2.0 §9.2). Given a
same-key edit, the obvious implementation is "remove the record, then add the
new one".

**That is wrong**, because each of those operations builds its new item payload
from the **original** chain. Applied to one key in one transaction, the second
operation silently discards the first's result — the last writer wins and the
chain is corrupted.

So v2.3 adds an internal primitive, `embkfs_dirent_retarget_op()`, which
performs the whole edit **as a single item rewrite**: it reads the chain once,
produces the new payload once, and writes it once. Any implementation that
supports rename must do the same, or handle collision chains some other way that
is closed under composition.

### 1.2 Known limitation (deliberate, not a bug)

A rename **between two names that hash-collide inside the same directory** is
refused (`-EINVAL`) rather than done wrong. Source and destination would live in
the same chain item, and the retarget primitive edits one record at a time. The
refusal is honest and loud; the frequency is `1/2^32` per pair.

## 2. Other operations added since v2.2

| Operation | Kernel entry | Notes |
|---|---|---|
| `chmod` | `embkfs_chmod_object()` | mode-only inode update. `git init` probes `core.filemode` by chmod-ing a probe file and stat-ing it back. |
| `ftruncate` | via `vfs_fd_truncate` | grow and shrink; shrink frees whole extents. |
| real `unlink` | — | v2.2's stub was a **stale lie**: it returned success without removing anything. Fixed. |
| `/dev/null` | `FD_BACKING_NULLDEV` (fd layer) | **Not an EMBKFS object.** Special-cased in `vfs_open` before path resolution — there is no device-node type on disk, and inventing one to satisfy a name would be worse than the special case. |

## 3. Formatter policy: bounded extents (`EXTENT_MAX_BYTES = 1 MiB`)

**Not a format change** — the format has always allowed many extents per file.
This is what the formatter *chooses*, and it is spec-relevant because the old
choice made large files pathological to read.

**The problem.** mkfs used to emit **one extent per file**, and an extent's
checksum covers the **whole extent**. So nothing in a file could be served until
*all* of it had been read and CRC'd: a single 8 KB read of the 10.3 MB
`python314.zip` cost O(filesize). Cold read amplification was **443%** — the
reader "CRC'd 2 GB before byte 0".

**The fix.** `build_extent_items()` splits file data into extents of at most
**1 MiB**. Cold amplification fell to **194%**.

**Do not "improve" this to 64 KiB from first principles — it was A/B'd and it is
worse.** Measured over a real boot:

| Extent bound | device reads | of which B-tree node reads | total device time |
|---|---|---|---|
| 64 KiB | 8535 | 8336 | 10860 ms |
| **1 MiB** | **5401** | **5152** | **5886 ms** |

Smaller extents hit a lovely 101% cold amplification on file data — and cost
**85% more device time system-wide**, because **~95% of device reads on a real
workload are B-tree node reads**, and more extents means more extent items means
a bigger tree. Optimising the visible number made the system slower.

Extent lookup within a file is a **binary search** over the (sorted) extent
array, not a linear scan — with up to `filesize / 1 MiB` extents, the scan showed
up.

## 4. The read path (implementation, no format impact)

A second implementation is free to ignore all of this; it is recorded because
the numbers are the whole reason the OS can host CPython and git at all.

**36000 ms/MB → ~250 ms/MB warm.** The disk went from dominating wall-clock to
**1–4%** of it. Warm steady state: 48 requests for 3 MB, 64 KB each, **100%
amplification — zero waste**.

| Mechanism | Constant | What it fixes |
|---|---|---|
| inode cache | `embkfs_inode_cached()` | re-resolving an inode per read |
| extent cache | `embkfs_extents_cached()` | shared, cache-owned extent array |
| windowed read-ahead | `EMBKFS_WCACHE_WIN` = 64 KiB | the read-ahead the whole-object cache could not do |
| whole-object cache | `EMBKFS_RCACHE_MAX` = 8 MiB | small files; **the 10.3 MB zip got ZERO caching from it** — hence the window |
| batched device reads | `EMBKFS_IO_BATCH_BLOCKS` = 16 | per-request overhead in steady state |

**The old "request_size / 2.7 ms law" from earlier notes is WRONG.** Killed by
measurement, along with bigger block bounce buffers, overlapped ATA I/O, and the
ecache-thrash theory. Seven plans missed by optimising unmeasured layers.
**Run `test ioperf` before touching this path**, and measure the layer before
optimising it — one counter, one boot, every time.

## 5. Clarifications

- **Blocks 1..15 are FREE space and the allocator will use them.** Only block
  **0** is permanently reserved — an all-zero `block_ptr` is the "points at
  nothing" sentinel, so no real node or data block may live there. Plus the
  primary superblock (block 16) and the backup (last block). The formatter's map
  (16 = superblock, 17 = root, 18.. = leaves then data) never *claims* 1..15,
  and `embkfs_bitmap_build()` never marks them — so a file landing at block 1 is
  correct, not corruption. (It looks alarming in a hex dump. It isn't.)
- **The bitmap is always recomputed exactly from the live tree**, so the
  superblock's `free_blocks` is a **hint**, not an authority. A reader must not
  trust it.
- **The root directory is FLAT — mkfs packs no directories**, and that is now
  load-bearing rather than incidental: the on-OS C compiler finds `crt0.o`,
  `syscalls.o` and `libc.a` (6.6 MB, the largest object on the image) at `/` via
  `-L/`. The flat root *is* the path search. See [PORTS.md](PORTS.md).
- **The image is 64 MB** (`python.elf` 7.8 + stdlib zip 10.3 + `git.elf` 3.5 +
  `libc.a` 6.6 already). The budget is now a design input.
- **Single-leaf overflow is real**: enough packed objects push the metadata past
  one 4 KiB leaf, and `build_btree` auto-splits to a level-1 root. The
  hash-collision fixtures were dropped from the default image when it overflowed.

## Validation status

Every claim above is covered by a live selftest in `kernel/selftests.c`:

```
test posix                  10 rename checks incl. atomic-replace
                            (destination holds the SOURCE's bytes, size 4 not 14)
test git repo               init/config/add/commit/log/status -> a real root commit
                            (the lockfile-rename protocol, end to end)
test ioperf                 the read path's counters -- run it BEFORE touching §4
test embkfs all             the v2.0-v2.2 regression sweep, unaffected by v2.3
test tcc link               libc.a resolving from the flat root (§5)
```

`verify_embkfs.py` remains the independent oracle: it re-reads what the kernel
wrote without sharing its code, which is how a reader bug is caught rather than
agreed with.
