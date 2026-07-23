# EMBKFS Specification — Additions (v2.1 → v2.2)

> **Superseded in part by [`EMBKFS_spec_v2.3.md`](EMBKFS_spec_v2.3.md)**, which
> adds atomic `rename` (git's lockfile protocol depends on it), `chmod`,
> `ftruncate`, bounded extents, and the rebuilt read path. v2.3 changes no
> on-disk structure — everything in this document still stands.

This document layers EMBKFS v2's new capabilities on top of v2.0
(`EMBKFS_Specification_v2.0.pdf`) and v2.1's corrections
(`EMBKFS_spec_v2.1.md`), following the same discipline: every structure here
is exactly what the formatter (`mkfs_embkfs.py`), the independent verifier
(`verify_embkfs.py`), and the kernel reader (`kernel/fs/embkfs/embkfs.c`)
actually implement, cross-checked against each other. The on-disk
`version_major` stays **1** — every addition below is gated by a
`feature_incompat` bit (or lives in already-reserved space), so an
unaware reader degrades safely (refuses to mount) rather than silently
misreading the volume. This is genuinely additive, not a redesign.

Seven numbered sections below, one per phase of the v2.2 work. Each ends
with its own validation status (the selftest(s) that exercise it, runnable
via `test embkfs <name>` at the kernel shell, and `test embkfs all` for the
full regression sweep).

---

## 1. Real timestamps (RTC driver)

**What changed:** `atime`/`mtime`/`ctime`/`btime` (inode offsets 40/48/56/64,
per v2.1 §1) were always zero before this work — no RTC driver existed. They
are now sourced from `kernel/drivers/rtc.c`, a from-scratch CMOS RTC reader
(ports `0x70`/`0x71`), converted to a Unix epoch nanosecond count via
Howard Hinnant's public-domain `days_from_civil` algorithm.

**Precision caveat (deliberate, not a bug):** CMOS RTC hardware resolution
is **one second**. `rtc_now_ns()` returns `rtc_now_unix() * 1_000_000_000`
— real nanosecond *storage* (matching the inode field width), but not real
nanosecond *resolution*. Two timestamps taken less than a second apart can
read identical. Selftests that need to observe a timestamp *advance*
insert a real delay (`pit_delay_ms`) between operations rather than
assuming sub-second granularity.

**Update policy:**
- `btime` — set once, at object creation, never touched again.
- `mtime`/`ctime` — updated together on every content-mutating write
  (`embkfs_write_file`, the single funnel every public write API routes
  through) and, separately, `ctime`-only on metadata-only changes
  (link-count changes, etc.) — the standard POSIX mtime-vs-ctime split.
- `atime` — **not implemented as a live "last read" timestamp.** Real
  filesystems' own experience with `atime` (a metadata write on every
  read, CoW-rewriting a block just to record that it was looked at) is
  exactly why `relatime`/`noatime` mount options exist. `atime` is set
  once at creation (equal to `btime`) and never updated by reads. This is
  a deliberate scope decision, not an oversight — revisit only if
  something concretely needs a live `atime`.

**Also fixed along the way:** four pre-existing gaps where a parent
directory's own inode (`mtime`/`ctime`/`generation`) was never updated on
child create/rename/unlink/link, discovered while wiring timestamps
through every mutating call site. Real bugs, unrelated to the new RTC
work itself, fixed in the same pass since they touched identical code.

**Validation:** `test rtc` (plausible-range + monotonicity check against a
real PIT delay), `test embkfs timestamps` (creation timestamps equal and
non-zero, parent mtime advances on child creation, mtime/ctime advance on
write while btime stays fixed).

---

## 2. Multi-volume mounting + the ATA secondary-channel fix

**Mount table:** `embkfs_init()` no longer stops at the first EMBKFS
superblock it finds. It probes every registered block device (up to
`EMBKFS_MAX_VOLUMES = 4`) and mounts every one that validates. The first
mounted volume is `/` (aliased by `g_embkfs_live`, unchanged for every
existing single-volume caller); each additional volume is registered in
the VFS at `/<device_name>` (e.g. `/sdc`).

**A real, pre-existing bug found and fixed along the way:** the ATA driver
(`kernel/drivers/ata.c`) only ever routed the *primary* IDE channel's
completion interrupt (IRQ14) and always used the *primary* channel's
Bus-Master DMA registers, even for drives on the *secondary* channel. Any
I/O against a 3rd or 4th drive (secondary master/slave) would issue the
command, then block forever in `ata_wait_irq()` waiting for a completion
interrupt that could never arrive on that channel — eventually timing out
after roughly **1,000,000 100Hz `hlt`-wakeups (~2.7 hours)**, which is
indistinguishable from a dead hang against any normal test or boot
timeout. Fixed by:
- registering + routing IRQ15 (`ioapic_route(15, 47, ...)`) alongside the
  existing IRQ14 routing;
- separate per-channel completion flags (`ata_irq_fired_primary` /
  `_secondary`), so a primary-channel interrupt can't be mistaken for a
  pending secondary-channel completion or vice versa;
- a `bmide_channel_base()` helper: the secondary channel's Bus-Master
  command/status/PRDT registers live at `BAR4 + 0x08`, not `BAR4 + 0x00`.

This was blocking, not cosmetic — real multi-volume mounting (more than 2
physical drives) was unusable until this fixed itself out from under the
Phase 1 verification work.

**Validation:** `test embkfs multivol` (two independently-mounted volumes:
each independently listable, a write to one doesn't change the other's
generation counter) — needs a real 2nd EMBKFS device at boot, so it
degrades to a documented `SKIP` (not a `FAIL`) when only one volume is
present, e.g. plain `make run`. `make run-multivol` boots with a real
3-drive configuration (exercising the ATA fix directly) so the test has
something to verify against. `make run-usb-embkfs` mounts an
EMBKFS-formatted image over the xHCI mass-storage path specifically (its
own separate MSC implementation from the legacy UHCI/OHCI/EHCI stack).

---

## 3. Crypto primitives (`kernel/crypto/`)

A from-scratch primitives library — SHA-256, HMAC-SHA256, PBKDF2-HMAC-SHA256,
AES-256, and AES-256-XTS — built once in Phase 2 and reused by both
encryption (§5) and the verified-root boot check (§7). No third-party code;
every primitive is verified against **externally-computed known-answer
vectors**, not just internal round-trip self-consistency (the classic
from-scratch-crypto failure mode: a self-consistent-but-wrong cipher).

- **SHA-256** (`sha256.c`) — FIPS 180-4, standard 64-round compression.
  Verified against `hashlib.sha256()` (empty string, `"abc"`, and NIST's
  standard two-block message).
- **HMAC-SHA256** (`hmac.c`) — RFC 2104 construction. Verified against
  RFC 4231 test case 1.
- **PBKDF2-HMAC-SHA256** (`pbkdf2.c`) — RFC 8018. Verified against
  `hashlib.pbkdf2_hmac()` at `c=1` and `c=4096`, plus a **definitional**
  cross-check (1-iteration PBKDF2 must equal one direct HMAC call by
  RFC 8018's own definition — true regardless of any external vector,
  catching a wrong loop-init/block-index bug even if a memorized vector
  were ever mistyped).
- **AES-256** (`aes.c`) — FIPS-197. The S-box/inverse-S-box tables are
  **generated from first principles** (GF(2^8) multiplicative inverse +
  the standard affine transform) rather than transcribed from memory, then
  checked against the canonical `sbox[0x00]==0x63`/`sbox[0x53]==0xed`
  values. Verified against the FIPS-197 Appendix C.3 known-answer vector,
  cross-checked against Python's `cryptography` package
  (`algorithms.AES` + `modes.ECB`), plus a 64-trial round-trip sweep
  across varied keys/plaintexts.
- **AES-256-XTS** (`xts.c`) — IEEE P1619 / NIST SP 800-38E. Design
  decisions specific to this project (§5 explains why):
  - the tweak is the extent's own on-disk **block number**, little-endian
    in the low 8 bytes of a 16-byte buffer (high 8 bytes zero) — no
    stored nonce needed;
  - **no ciphertext stealing** — every call site always encrypts a whole
    multiple of the block size, so XTS's mechanism for a trailing partial
    block is simply never needed.

  Verified against 4-AES-block known-answer vectors independently
  computed via Python's `cryptography` package (`modes.XTS`) using this
  exact tweak encoding, plus differential checks (changing the block
  number, or one byte of one block's plaintext, must change that block's
  ciphertext and *no other* block's).

**Validation:** `test crypto sha256|hmac|pbkdf2|aes|xts`, or `test crypto
all` for the full sweep.

---

## 4. Per-extent compression

**Format:** `EMBKFS_EXTENT_F_COMPRESSED = 0x2` (extent flags, offset 40,
next bit after `HOLE = 0x1`). When set, `reserved1[0..7]` (of the extent
item's 16 reserved bytes, offset 48) holds `compressed_size` — the actual
compressed payload length in bytes, within the block-rounded `length`
run. `logical_size` (offset 16) keeps its existing meaning: the true,
decompressed byte count. A new `feature_incompat` bit,
`EMBKFS_INCOMPAT_COMPRESSION = 0x1`, is set on the superblock the first
time a write actually produces a compressed extent (not at format time
unconditionally) — an older/non-aware reader refuses the volume instead
of silently serving compressed bytes as if they were plaintext.

**Codec (`kernel/fs/embkfs/embkfs_compress.c`):** an LZ77-family
compressor, **LZ4-inspired, not byte-exact LZ4**. The token layout
(literal-length nibble + match-length nibble + 255-byte length extension,
exactly like real LZ4) is the same idea, but this format has **no
end-of-block marker** — `embk_decompress()` is always told the exact
expected output length up front (the extent's `logical_size` is already
on disk), so the decoder simply stops once it has produced that many
bytes rather than needing a specific "final sequence" encoding. This is a
deliberate simplification for a closed, single-implementation format, not
an attempt at LZ4 interop.

**Write policy:** compression is only attempted for a candidate extent's
logical span **≥ 2 blocks** (below that, no amount of compression can
reduce the block count by even one — the smallest possible win — so
attempting it is pure overhead). The write path computes the candidate
extent's `got` blocks exactly as before (allocator-driven, unchanged from
v2.1), *then* attempts compression on that span; if the compressed form
needs strictly fewer blocks, the unused tail is returned to the allocator
via `embkfs_note_freed_run()` before the transaction commits, and the
extent's `length` shrinks to what the compressed payload actually needs.
If compression doesn't help (or isn't attempted), the write is byte-for-
byte identical to v2.1's behavior — this is the "only keep it if it
helps" policy, and it's verified to actually run (not just exist) by a
dedicated incompressible-data test case.

**Checksum semantics:** the extent's `checksum` (offset 24) covers the
**on-disk (compressed) bytes**, not the logical bytes — verified before
any decompression on read, matching the project's existing "verify what's
actually on disk first, then interpret it" principle (spec 9.3). This is
why `embkfs_extent_validate()` needed a real change: the old invariant
`logical_size <= length * block_size` is exactly what compression
violates on purpose (that's the point), so the check now branches —
compressed extents validate `compressed_size <= length * block_size`
instead, plus a volume-size sanity bound on `logical_size` so a corrupt
extent can't make the read path allocate something absurd.

**Read path:** no random-access decode — producing *any* logical byte of
a compressed extent requires reading, checksum-verifying, and
decompressing the *whole* compressed blob first, then slicing out
whatever the caller actually wanted. This applies even to a short prefix
read of a large compressed extent (a real, if minor, cost of this simple
scheme's lack of block-level seekability within one extent).

**Validation:** `test embkfs compress` runs both the standalone codec's
own known-answer/round-trip tests (`embk_compress_run_selftests` — all-zero,
repeating-pattern, and pseudo-random data) and a filesystem-level
integration test: writes real files through the live mount and confirms
**block usage actually shrank** (via the allocator's free-block count) for
compressible data, and did **not** shrink for incompressible data above
the size threshold — proving the "only keep it if it helps" policy is
live, not just present in the code.

---

## 5. Encryption (AES-256-XTS + passphrase unlock)

**Superblock crypto header (`struct embk_crypto_header`, 52 bytes):**
lives at a **fixed offset (200) within the same 512-byte sector** the
superblock struct itself occupies (both superblock copies are already
read/written as one sector each, so this needed no extra I/O to add) —
but **past** `EMBKFS_SB_BODY_SIZE` (152), so it is **not** covered by the
superblock's own checksum.

| Offset | Size | Field |
| -----: | ---: | ----- |
| 0 | 8 | magic (`EMBKFS_CRYPTO_HEADER_MAGIC`, "EMBKCRY1") |
| 8 | 16 | `kdf_salt` |
| 24 | 4 | `kdf_iterations` |
| 28 | 16 | `key_check_ciphertext` |
| 44 | 8 | reserved |

**This is a deliberate scope reduction, not an oversight:** corruption of
the crypto header fails **closed** (mount refused, or a correct
passphrase gets wrongly rejected) — it costs *availability*, never
*confidentiality or integrity* of the actual encrypted data, since the
data's own per-block checksums (still CRC32C, computed over ciphertext,
completely independent of this header) catch corruption there regardless.

**Feature gate:** `EMBKFS_INCOMPAT_ENCRYPTED = 0x2` (superblock
`feature_incompat`) — set at format time, never dynamically.

**Key derivation and check:** PBKDF2-HMAC-SHA256(passphrase, `kdf_salt`,
`kdf_iterations`, 64 bytes) → `[0:32)` is the XTS data key, `[32:64)` is
the XTS tweak key. `key_check_ciphertext` is those keys' XTS encryption
(tweak = all-zero 16 bytes) of a fixed, non-secret 16-byte constant
(`"EMBKFS-KEY-CHECK"`) — a standard LUKS-style key-check: the passphrase
is confirmed by re-deriving and re-encrypting, without ever storing the
real key or the passphrase itself on disk.

**Mount-time flow:** on seeing `EMBKFS_INCOMPAT_ENCRYPTED`,
`embkfs_mount()` prompts at the keyboard (masked echo — `*` per
character, blocking `keyboard_getchar()`, since this runs before the
shell's own polling input loop exists), derives candidate keys, checks
against `key_check_ciphertext`, retries up to 3 times, then refuses
**only this volume** (`-EMBK_EACCES`) — `embkfs_init()`'s per-device mount
loop moves on to the next block device regardless, so one encrypted (or
mistyped) volume never blocks any other, unencrypted volume from
mounting.

**Extent metadata + write ordering:** `EMBKFS_EXTENT_F_ENCRYPTED = 0x4`.
Order is **compress, then encrypt** — §4's compressed bytes (or the plain
logical bytes, if compression didn't help) become the plaintext input to
encryption, always the *last* transform before a block hits the platter.
Encryption operates on the **whole physical block** (`vol->block_size`),
including whatever zero-padding trails a partial final block — each
physical block is its own independent XTS "sector," tweaked by its own
disk block number, regardless of which extent or file it belongs to.

**Checksum semantics (encrypted extents):** because encryption turns the
zero-padding into ciphertext too, an encrypted extent's checksum covers
the **whole `block_size` per block**, not just the `chunk` of real bytes
— unlike an unencrypted (or compressed-only) extent, which has no
real-vs-padding distinction worth making on read. The read path computes
this checksum **before** decrypting (over the raw on-disk ciphertext,
matching what write time hashed) — decrypting first and then checksumming
would hash the wrong bytes and fail verification on every single block.

**Validation:** `test embkfs crypto`-equivalent is folded into `test
embkfs compress` when run against an encrypted volume (proving
compression and encryption compose correctly on the same extent) plus a
direct boot test (`make run-embkfs-encrypted`): the kernel prompts,
accepts the correct passphrase, mounts, and the file's real content reads
back correctly. **Independent oracle:** `verify_embkfs.py --passphrase
<pw>` decrypts using Python's `cryptography` package — a completely
separate AES-XTS implementation from the kernel's own — derives the same
keys via `hashlib.pbkdf2_hmac`, and confirms byte-for-byte content
matches, with a wrong passphrase correctly rejected via the key-check
ciphertext mismatch.

---

## 6. OS-native features

### 6a. Self-healing dual-superblock repair

Extends the existing mount procedure (read primary, verify checksum, fall
back to backup, compare generations if both are valid) with one more
step: once the authoritative copy is chosen, if the **other** copy is
either invalid (bad checksum/magic) or valid-but-at-an-older-generation
(a crash between writing the primary and the backup mid-commit — the two
are written *sequentially*, not atomically as a pair), the authoritative
copy's **full block** is written over the stale/bad one. A repair
failure is only logged — the volume still mounts fine from the good copy
regardless; this can never block the mount itself.

**Validation:** `test embkfs selfheal` corrupts the live volume's
on-disk backup superblock (a single bit-flip in its checksum field) and
re-mounts the same device into a scratch volume struct — exercising the
*real* mount-time repair path a boot would run, not a synthetic stand-in.
Confirms the backup comes back byte-identical to the primary, and that a
second remount (both copies now agreeing) still mounts cleanly with no
further repair needed.

### 6b. Instant snapshots + snapshot-aware allocator

**A snapshot is a frozen root `block_ptr`, nothing more** — taking one is
O(1), because the tree it points at is already immutable the instant a
commit lands (that's what copy-on-write means). Stored as ordinary tree
items: `{EMBKFS_ROOT_OBJECT_ID, EMBK_TYPE_SNAPSHOT (64), slot}`, reusing
the exact same transactional put/commit machinery as everything else
rather than inventing a separate on-disk registry format. Up to
`EMBKFS_MAX_SNAPSHOTS = 16` slots, a small fixed linear range (not a name
hash like directory entries — with only 16 possible slots, a direct
per-slot probe is simpler than maintaining a free-list and needs no
collision-chain handling).

`struct embk_snapshot_item` (80 bytes):

| Offset | Size | Field |
| -----: | ---: | ----- |
| 0 | 32 | `name` (NUL-padded, not guaranteed NUL-terminated if the name uses the full 31 usable bytes) |
| 32 | 32 | `root` (the frozen `block_ptr`) |
| 64 | 8 | `generation` (live generation at snapshot time) |
| 72 | 8 | `timestamp` (`rtc_now_ns()` at snapshot time) |

**The allocator dependency (the real work, not optional):**
`embkfs_txn_end()`'s reconciliation of a successful commit's superseded
blocks (`t->freed` — old tree nodes, and `t->frun` — old data extents)
now checks `vol->snapshot_count`. While it's nonzero, **no freed block is
reclaimed at all**, regardless of whether it's actually still needed by a
retained snapshot.

**Simplification (documented, not silently approximated):** this is a
coarser policy than true per-block reference counting — it doesn't track
*which* snapshot needs *which* block, just "hold everything while
anything is retained." Always safe (never frees something a snapshot
needs); costs delayed reclaim of blocks that happen to be freed while
*any* snapshot exists, even ones that snapshot doesn't actually
reference. Deleting the **last** snapshot triggers a full
`embkfs_bitmap_build()` (walks the live tree fresh) to recover everything
that's now genuinely free, rather than waiting for the next remount. True
refcounting is a natural v2.x follow-up.

**Mount-time (and rollback-time) protection:** `embkfs_bitmap_build()`
marks blocks reachable from the live root **and** every currently-
registered snapshot's own root — otherwise a remount would incorrectly
see a snapshot-only block as free and hand it out again.

**A real bug found and fixed during this phase:** `embkfs_snapshot_create()`'s
own commit CoW-rewrites the tree to add the snapshot's registry entry —
which *supersedes the very tree nodes the new snapshot's frozen root now
points at*. The naive ordering (commit, then increment
`vol->snapshot_count`) let that commit's own `txn_end` free those nodes
immediately, since the count was still zero *during* the commit that
creates the snapshot. Fixed by incrementing the count **before**
reconciling that specific transaction's frees (rolled back if the commit
itself fails) — caught by `test embkfs snapshot`'s free-count assertions
before it could reach a release image.

**THE ROLLBACK LIMITATION (v2.2), and its fix (v2.3).** Because the v2.2
registry lives *inside* the same versioned tree it is tracking versions
of, rolling back to an older snapshot reverts the **entire** tree,
including the registry — any snapshot taken *after* the rollback target
stops existing in the restored tree (the snapshot rolled back *to* also
loses its own entry, since that entry was necessarily written by a
*later* commit than the one it records). The same way `git reset --hard`
drops refs that lived only in now-abandoned history. Rollback was a
one-way door: you could go back, never forward again.

### v2.3 — the registry moves out of the tree (`INCOMPAT_SNAPREG`)

The registry now lives in **one fixed block (block 1)** that no
transaction rewrites. Rollback swaps the root and does not touch it, so
snapshots on **both sides** of the target survive and rollback becomes
navigable in both directions.

| | v2.2 (legacy) | v2.3 (`INCOMPAT_SNAPREG`) |
|---|---|---|
| Registry location | items in the CoW tree | block 1, outside it |
| Create/delete cost | a full CoW commit | one block write |
| After rollback | newer snapshots gone | all snapshots intact |
| Integrity | tree node checksums | own CRC32C over the block |

*Why a fixed block, not superblock-adjacent bytes.* The crypto (offset
200) and verify (260) headers share the superblock's 512-byte sector,
but 16 slots × 80 bytes = 1280 does not fit; shrinking the entry to make
it fit would have cost either the name length or the snapshot count. A
fixed block costs one of the 15 blocks the formatter already leaves
unused before the superblock. It is fixed rather than allocated for the
same reason the superblock's location is: a pointer to it would need to
live somewhere durable, and a registry you must already have the
registry to find is no better off.

*Why INCOMPAT rather than ro_compat.* A reader that does not know the
bit would see block 1 as free and hand it to the allocator, overwriting
the registry. Refusing the mount is the only safe answer. `mkfs` sets
the bit and formats the block; `embkfs_bitmap_build()` reserves it
alongside the superblock and the backup, and the free-block hint counts
it (`used = 4 + …`), so the mount-time allocator oracle still agrees
exactly.

*Integrity.* The block carries its own CRC32C over everything after the
checksum field, the node-header convention. A registry that fails magic
or checksum is reported as **empty**, not guessed at: a bogus root
pointer would send `embkfs_bitmap_build()` marking arbitrary blocks
in-use, which is worse than losing the list. `verify_embkfs.py` §1a
recomputes that checksum independently and range-checks every root.

*Legacy volumes still work.* Without the bit, both kernel and verifier
use the in-tree registry and the v2.2 limitation above still applies —
because on those volumes it is still true.

**Proof:** `test embkfs snapreg` — write A, snapshot s1, write B,
snapshot s2, write C; roll back to s1 (content A); **assert s2 still
exists**; roll *forward* to s2 (content B); roll back to s1 again; then
confirm the registry survives a full bitmap rebuild. Rolling forward is
the step that could not previously happen at all, and it proves more
than the registry: s2's frozen tree must still be physically intact,
i.e. its blocks stayed marked in-use across a rollback that made them
unreachable from the live root.

Blocks orphaned by a rollback (writes made *after* the snapshot point,
held by no snapshot) are still correctly reclaimed by the same
bitmap-rebuild mechanism, since they are reachable from neither the
restored root nor any registry entry.

**Shell:** `snap create|list|delete|rollback <name>`.

**Validation:** `test embkfs snapshot` — write, snapshot, overwrite,
confirm the old extent is held (both via the live free-block count *and*
an explicit forced bitmap rebuild, proving mount-time protection
specifically, not just the txn_end hold-back), rollback, confirm the
*original* bytes return.

### 6c. Process-provenance tracking

`writer_pid` (4 bytes) lives in the inode's `reserved[0..3]` (offset 80).
Sourced from `current_process->pid` (guarded by `current_thread` — the
macro `current_process` itself dereferences `current_thread`, so it must
be checked first, not the derived macro) at every content-mutating call
site: `embkfs_make_object()` (creation) and `embkfs_write_file()` (every
write) — the same two sites Phase 0's `btime`/`mtime` distinction already
uses, since "who wrote the DATA" tracks the same event as "did the DATA
change." `0` means unknown (no process context — an inode from an image
written by the mkfs oracle, which has no process to record).

**Shell:** `stat <path>` — size/blocks/links/mode, all four timestamps,
and `writer_pid` (or an explicit "unknown" if zero).

**Validation:** `test embkfs provenance` confirms a created/written
object's `writer_pid` matches `current_process->pid` at the time of the
call. Testing from two genuinely *different* processes would need real
multi-process spawning — out of scope for this unit-level check, which
verifies the mechanism itself (reading `current_process->pid` at the
right call sites); that mechanism generalizes trivially to whichever
process is really calling.

### 6d. Verified-root boot check

**Scoped honestly, flagged explicitly:** this is **HMAC-SHA256
authentication with a key embedded in the kernel binary**
(`EMBKFS_VERIFY_ROOT_KEY`, `embkfs.c`), not real asymmetric signing.
Anyone with a copy of this kernel's source or binary can compute a valid
HMAC for any root they like — it does **not** defend against an attacker
who has the kernel image, only against **offline tampering** by someone
who doesn't (a raw-disk edit, a swapped drive, a different/unmodified
kernel's write path touching the volume). A true asymmetric upgrade
(Ed25519 or similar) is a natural v2.x follow-up — itself another
crypto-primitive-sized body of work on top of §3's SHA-256/AES,
deliberately not attempted here rather than silently passed off as more
than it is.

**Feature gate:** `EMBKFS_INCOMPAT_VERIFIED_ROOT = 0x4` — an **INCOMPAT**
bit specifically (not ro_compat/compat): the entire point is that an
implementation which doesn't enforce the check must **refuse** the
volume, not silently skip verification. A ro_compat/compat bit would make
bypassing the check as simple as mounting with an older reader.

**Verify header (`struct embk_verify_header`, 48 bytes),** offset 260
within the same sector as the superblock struct (past the crypto
header's 200–252, no overlap):

| Offset | Size | Field |
| -----: | ---: | ----- |
| 0 | 8 | magic (`EMBKFS_VERIFY_HEADER_MAGIC`, "EMBKVER1") |
| 8 | 32 | `hmac` = HMAC-SHA256(key, `root` (32B) ‖ `generation` (8B)) |
| 40 | 8 | reserved |

Authenticates exactly the 40 bytes that change on every commit — nothing
else in the superblock needs covering (`block_size`/`total_blocks`/`uuid`
are fixed at format time; `free_blocks` is already implied by
generation+root together, so including it would be redundant weight on
every recompute).

**Enforced by the kernel, immediately post-mount-validation, before
anything is trusted** (not the real-mode stage1/stage2 bootloader — a
512-byte boot sector has no room for a crypto library, so "verified boot"
here means "the OS refuses to trust a tampered volume before executing
anything from it," a real and useful property, just not a hardware root
of trust). `embkfs_write_superblock()` **re-signs the HMAC on every
commit** (same atomic write as generation/root/free_blocks/
feature_incompat) — this kernel has the embedded key, so it keeps the
stored HMAC in sync with its own legitimate writes automatically; only
tampering by something *without* the key ever makes a later mount's check
fail.

**Validation:** `test embkfs verifyboot` crafts a superblock with
`VERIFIED_ROOT` set and a correct HMAC directly on the live device (never
touching the already-mounted live volume's in-memory state), re-mounts it
into a scratch struct — must succeed. Then flips one bit of the
authenticated root (the stored HMAC now stale) and re-mounts again — must
be refused, specifically with `EMBK_EACCES`. Restores the original
on-disk bytes afterward either way.

---

## 7. Summary of new feature-flag bits

All `feature_incompat` (superblock offset 32) — an unaware reader refuses
to mount a volume with any bit it doesn't recognize, by the format's
existing (v2.0) feature-negotiation rule:

| Bit | Value | Meaning |
| --- | ----: | ------- |
| `EMBKFS_INCOMPAT_COMPRESSION` | `0x1` | ≥1 extent on this volume is LZ-compressed |
| `EMBKFS_INCOMPAT_ENCRYPTED` | `0x2` | volume requires the passphrase-unlock flow |
| `EMBKFS_INCOMPAT_VERIFIED_ROOT` | `0x4` | volume requires the HMAC boot check |

All three compose freely (an extent can be compressed *and* encrypted; a
volume can be encrypted *and* verified-root *and* opportunistically
compressed, all at once) — verified directly by `test embkfs compress`
run against an encrypted volume in the Phase 4 work.

Extent-level flags (offset 40, independent bit space from the above):

| Bit | Value | Meaning |
| --- | ----: | ------- |
| `EMBKFS_EXTENT_F_HOLE` | `0x1` | sparse run, no disk block (v2.0) |
| `EMBKFS_EXTENT_F_COMPRESSED` | `0x2` | `reserved1[0..7]` holds `compressed_size` |
| `EMBKFS_EXTENT_F_ENCRYPTED` | `0x4` | checksum covers the whole padded block, not just real bytes |

---

## Validation status

Every feature above lands with its own selftest, folded into `test
embkfs all`'s full regression sweep (`kernel/selftests.c`):

```
test rtc
test embkfs timestamps
test embkfs multivol        (SKIP without a real 2nd volume at boot)
test crypto sha256|hmac|pbkdf2|aes|xts   (or: test crypto all)
test embkfs compress
test embkfs selfheal
test embkfs snapshot
test embkfs provenance
test embkfs verifyboot
```

plus the pre-existing v2.0/v2.1 suite (`path`, `alloc`, `tree`, `obj`,
`ns`) unchanged and still green throughout. Oracle-side (Python)
verification stays independent per the project's standing discipline:
`mkfs_embkfs.py`/`verify_embkfs.py` were updated alongside every
on-disk-format change in this document, never after, including a
from-scratch Python port of the compression codec's decoder and an
XTS decrypt path built on the `cryptography` package (never reusing the
kernel's own crypto code) so the oracle can't share a bug with the
implementation it's checking.
