# EMBX (EmbLink Binary eXecutable) — Specification v2

**Status:** design, byte-exact. No implementation exists.
**Supersedes:** `EMBX_Specification_v1.md` (retained as the wishlist that produced
the capability question — see §11).
**Governing decision:** EmbCC `docs/DECISIONS.md` D-003, **reopened**. Building
EMBX is a learning goal — the next ring of "understand every line." *Adopting*
EMBX in EmbLinkOS remains governed by D-006 (capability, not authorship),
unchanged.

---

## 1. Scope

EMBX is **one container with N load contracts.** A single header, segment table,
checksum discipline and dumper (`embread`) serve every binary kind. The
*container* is defined once, byte-exact, for all kinds. A *load contract* — what
a loader does with the bytes — is defined only for kinds that have a loader
today.

| Kind | Ext | Container | Load contract |
|---|---|---|---|
| Application | `.embx` | ✅ v1 | ✅ **defined** (§4.1) |
| Shared library | `.embdll` | ✅ v1 | ⏳ deferred to v1.1 (§4.2) |
| Kernel module | `.embmod` | ✅ v1 | ⏳ reserved — loader refuses (§4.4) |
| Boot driver | `.embdrv` | ✅ v1 | ⏳ reserved — loader refuses (§4.4) |
| Firmware bundle | `.embfw` | ✅ v1 | ✅ **defined** as non-executable (§4.3) |
| Boot image | `.embboot` | ✅ v1 | ⏳ reserved — see the `paddr` finding (§4.5) |

Reserving all six kinds by name now costs one `uint16_t`. Defining semantics for
kinds with no loader would cost correctness — semantics that cannot be tested are
semantics that are wrong.

**The extensibility mechanism, stated once:** the header is a **fixed 128 bytes,
forever.** Everything else hangs off the segment table by segment *type*. Linkage
tables, TLS, notes and debug data arrive as new segment types, not new header
fields. A v1 loader reading a v1.1 binary walks past segment types it does not
know; it does not misparse a header it thought it understood.

---

## 2. Design rules

These are the rules the layout below is derived from. Where a field looks
arbitrary, the rule is the reason.

1. **The spec is the loader's read sequence, written down.** §6 is the algorithm;
   §3 is what the algorithm reads. Fields were not chosen and then given jobs.
2. **Every field answers three questions:** who reads it, at what point in §6,
   and what happens if it is wrong. A field with no reader at load time is not a
   format field — it is metadata, and metadata lives in a NOTE segment or a
   sidecar file.
3. **Little-endian, always.** There is no endianness field. EMBKFS made this
   choice and it was correct: a single-implementation format on a little-endian
   target that lets the *file* declare its byte order has invented a decode path
   that will never execute and can never be tested.
4. **Byte-exact or it is not a spec.** Every structure has fixed offsets, fixed
   widths, and a `_Static_assert` on its total size. Off by one byte and the
   build fails instead of the loader silently misreading.
5. **Deterministic output.** Nothing in the container records wall-clock time.
   EmbCC's M3 acceptance is `embcc-stage2` byte-identical to `embcc-stage1`; a
   producer that stamps the current time into a header can never pass its own
   fixed-point check. Identity is `build_id` — a content hash.
6. **One source of truth per fact.** Where two fields could encode the same
   thing, one of them is deleted (see §11: BSS, `min_kernel_version`, segment
   type vs. protection flags).
7. **Refuse loudly; never load partially.** Every violation in §8 is a
   parse-time rejection with a distinct error code, before a single byte is
   mapped. There is no repair path and no best-effort load.

---

## 3. The container

### 3.1 Header — 128 bytes, at file offset 0

| Offset | Size | Field | Notes |
|---|---|---|---|
| `0x00` | 8 | `magic[8]` | See below |
| `0x08` | 2 | `version_major` | `1` |
| `0x0A` | 2 | `version_minor` | `0` |
| `0x0C` | 4 | `header_size` | `128`. Must equal 128 in v1 |
| `0x10` | 2 | `binary_type` | §4 |
| `0x12` | 2 | `machine` | `1` = x86-64, `2` = arm64 |
| `0x14` | 4 | `abi_version` | EmbLink syscall/ABI contract version |
| `0x18` | 8 | `flags` | §3.5 |
| `0x20` | 8 | `feature_incompat` | §3.6 |
| `0x28` | 8 | `feature_compat` | §3.6 |
| `0x30` | 8 | `entry_point` | Virtual address. `0` for non-executable kinds |
| `0x38` | 4 | `segment_table_offset` | File offset. **Should** be `128` (§4.5) |
| `0x3C` | 2 | `segment_count` | |
| `0x3E` | 2 | `segment_entry_size` | `64`. Stride; lets a future parser skip safely |
| `0x40` | 4 | `capability_table_offset` | File offset, or `0` if `capability_count == 0` |
| `0x44` | 2 | `capability_count` | |
| `0x46` | 2 | `capability_entry_size` | `16` |
| `0x48` | 4 | `grantor` | **Reserved. MUST be 0 in v1** (§5.4) |
| `0x4C` | 4 | `reserved0` | MUST be 0 |
| `0x50` | 32 | `build_id[32]` | SHA-256, §3.4 |
| `0x70` | 8 | `image_size` | Total file size in bytes |
| `0x78` | 4 | `reserved1` | MUST be 0 |
| `0x7C` | 4 | `header_checksum` | CRC32C over bytes `0x00`–`0x7B` |

```c
_Static_assert(sizeof(struct embx_header) == 128, "EMBX header must be 128 bytes");

/* Derive the checksummed span from the struct so it can never drift from the
 * layout, exactly as EMBKFS_SB_BODY_SIZE does: 128 - 4 = 124. */
#define EMBX_HDR_BODY_SIZE (sizeof(struct embx_header) - sizeof(uint32_t))
```

**The magic, byte by byte** — every byte has a job:

```
7F 45 4D 42 58 0D 0A 1A     .EMBX\r\n\x1A
```

- `0x7F` — not a valid ASCII character. Marks the file as binary so a text tool
  cannot mistake it for text. (ELF's reason for `EI_MAG0`.)
- `"EMBX"` — human-readable in a hex dump. Costs nothing, saves an hour.
- `0x0D 0x0A` — a CRLF pair. If any naive text-mode transfer, editor or git
  filter mangles line endings, **the magic breaks loudly** instead of the binary
  being silently corrupted. This project has already been bitten by exactly this
  hazard: `.gitattributes` marks `*.img binary` because UTF-8 handling corrupted
  disk images. (PNG's reason for its signature.)
- `0x1A` — DOS EOF / SUB. Stops a naive `type`/`cat` from spraying the whole
  image at a terminal.

### 3.2 Segment table entry — 64 bytes

| Offset | Size | Field | Notes |
|---|---|---|---|
| `0x00` | 4 | `type` | §3.3 |
| `0x04` | 4 | `flags` | `R=1`, `W=2`, `X=4`. Meaningful for `LOAD` only |
| `0x08` | 8 | `vaddr` | Virtual address |
| `0x10` | 8 | `file_offset` | |
| `0x18` | 8 | `file_size` | Bytes present in the file |
| `0x20` | 8 | `mem_size` | Bytes in memory; `>= file_size` |
| `0x28` | 8 | `align` | Power of two, `>= 4096` for `LOAD` |
| `0x30` | 4 | `checksum` | CRC32C over the segment's `file_size` bytes |
| `0x34` | 4 | `reserved0` | MUST be 0 |
| `0x38` | 8 | `paddr` | **Reserved for the BOOT contract** (§4.5). MUST be 0 for all other kinds |

```c
_Static_assert(sizeof(struct embx_segment) == 64, "EMBX segment entry must be 64 bytes");
```

**There is no BSS segment type.** `mem_size > file_size` *is* BSS: the loader
zero-fills `[vaddr + file_size, vaddr + mem_size)`. Two ways to say one thing is
one way too many.

**Protection flags are the only source of truth for permissions.** `type`
answers *what the loader does with this segment*; `flags` answers *with what
permissions*. A segment table carrying both a `Code` type and `R+W` flags has
invented a contradiction the loader must arbitrate, and there is no correct
arbitration.

The per-segment `checksum` is deliberately per-segment rather than one
whole-image hash: it lets a loader validate incrementally, which a future
demand-paging loader and the boot loader both need, and it localizes corruption
to a segment rather than reporting "the file is bad."

### 3.3 Segment types

| Value | Name | Mapped? | Status |
|---|---|---|---|
| `0` | `EMBX_SEG_NULL` | no | Ignored entirely |
| `1` | `EMBX_SEG_LOAD` | **yes** | Defined, v1 |
| `2` | `EMBX_SEG_DYNAMIC` | no | Reserved v1.1 — linkage tables (§4.2) |
| `3` | `EMBX_SEG_TLS` | no | Reserved |
| `4` | `EMBX_SEG_NOTE` | no | Metadata. Producer-defined payload |
| `5` | `EMBX_SEG_DEBUG` | no | Not mapped; may be stripped |

An unknown segment type is **not** an error: the loader ignores it. This is the
half of the extensibility mechanism that `feature_incompat` does not cover — a
new *optional* section costs nothing, while a change that would make an old
loader misread the image sets an incompat bit and gets refused.

### 3.4 Checksums and build ID — the ordering procedure

Three integrity fields with three different jobs, and a fixed order of
computation so the definitions are not circular.

- `header_checksum` (CRC32C) — validates the header **before any offset inside
  it is trusted.** This is the bootstrap step: `segment_table_offset` is not
  usable until the header is known intact. CRC32C is the house function and
  already in the kernel.
- `image_size` — truncation detection. Every offset in the file is bounds-checked
  against it.
- `build_id` (SHA-256) — content *identity*, for provenance, dedup and a future
  signature. SHA-256 is already in `kernel/crypto/` from EMBKFS Phase 2.

**Producer, in this order:**

1. Write the whole image with `build_id` and `header_checksum` set to zero.
2. Compute and write each segment's `checksum`.
3. Compute `build_id` = SHA-256 over the entire file, with `build_id` and
   `header_checksum` both still zero. Write it.
4. Compute `header_checksum` = CRC32C over bytes `0x00`–`0x7B`. Write it. Done.

**Verifier:** check `header_checksum` over the first 124 bytes (it is the last
field, so nothing needs masking). To verify `build_id`, hash the image with
`build_id` and `header_checksum` zeroed in a working copy.

### 3.5 Image flags (`flags`, offset `0x18`)

| Bit | Name | Status |
|---|---|---|
| `0x1` | `EMBX_F_PIE` | Reserved. v1 producers MUST NOT set it |
| `0x2` | `EMBX_F_STRIPPED` | Informational: no `DEBUG` segments present |

### 3.6 Feature bits

The EMBKFS pattern, **adapted rather than copied.** EMBKFS has three classes —
compat, ro_compat, incompat. A binary has no analogue of `ro_compat`: it is
either executed or it is not, and there is no partial-trust execution mode. So
EMBX has two classes.

```c
#define EMBX_INCOMPAT_SIGNED     0x1ULL   /* reserved — v1 refuses */
#define EMBX_INCOMPAT_COMPRESSED 0x2ULL   /* reserved — v1 refuses */
#define EMBX_INCOMPAT_ENCRYPTED  0x4ULL   /* reserved — v1 refuses */
#define EMBX_INCOMPAT_LINKAGE    0x8ULL   /* v1.1: a DYNAMIC segment is present */

#define EMBX_KNOWN_INCOMPAT      0ULL     /* v1 understands none of them */

#define EMBX_COMPAT_DEBUG_SIDECAR 0x1ULL  /* a .embdbg exists alongside */
```

The rule, verbatim from EMBKFS: **any incompat bit outside `EMBX_KNOWN_INCOMPAT`
means refuse to load.** Compat bits are safe to ignore. This is what lets a
signed or compressed binary be introduced later without a v1 loader
half-understanding it — reserving the bit now is the entire cost of that
guarantee.

---

## 4. Load contracts

### 4.1 Application — `.embx`, `EMBX_TYPE_APP` = 1 — **defined**

- Fully linked, fixed virtual addresses, **no relocations**.
- `entry_point` must be nonzero and must fall inside a `LOAD` segment carrying
  `X`.
- Every `LOAD` segment's `vaddr` range must lie entirely below
  `0x0000800000000000` — the canonical low-half boundary `access_ok()` already
  enforces.
- No `DYNAMIC` segment. `capability_count` may be zero or more (§5).

### 4.2 Shared library — `.embdll`, `EMBX_TYPE_DLL` = 2 — **deferred to v1.1**

The container holds a `.embdll` today; the *linkage model* is not defined in v1,
deliberately. Defining an import/export/relocation format before EmbCC can emit
a relocation is designing against a producer that does not exist — the exact
inversion D-003 was reopened *with eyes open about*, and there is no reason to
repeat it inside the same document.

**When it lands it changes no header field.** Imports, exports, relocations and
the string table live in the payload of an `EMBX_SEG_DYNAMIC` segment, and the
producer sets `EMBX_INCOMPAT_LINKAGE`. A v1 loader refuses such a binary cleanly
instead of loading it without relocating it.

### 4.3 Firmware bundle — `.embfw`, `EMBX_TYPE_FW` = 5 — **defined, non-executable**

A container for device firmware, delivered to a subsystem-specific manager (a
USB, GPU or NIC driver), **never to the program loader.** The degenerate case is
defined precisely so that "not executable" is enforced rather than assumed:

- `entry_point` MUST be `0`.
- No segment may carry `X`.
- `capability_count` MUST be `0` — firmware declares nothing; the *driver*
  loading it holds the authority.
- The generic program loader, handed an `.embfw`, MUST refuse with
  `EMBX_ETYPE`. It does not attempt a best-effort load.

A subsystem manager reads segments by index and treats their bytes as opaque
payload; `vaddr` is meaningless and MUST be `0`.

### 4.4 Kernel module / boot driver — `EMBX_TYPE_MOD` = 3, `EMBX_TYPE_DRV` = 4 — **reserved**

Reserved by name. The program loader refuses both with `EMBX_ETYPE`.

The reason these cannot simply reuse §4.1 — and it is the finding that makes
"unified container" honest rather than a slogan — is that **the six kinds do not
share a relocation model.** An app is fully linked. A `.embdll` is PIC at a load
bias with two-way symbol binding. A `.embmod` is a *relocatable object*: it has
unresolved symbols against a kernel symbol table that does not exist yet, and it
relocates into kernel virtual address space under `-mcmodel=kernel`, where the
±2 GB displacement window from `0xFFFFFFFF80000000` is a hard load-time failure
mode with no userspace analogue.

One container, N contracts — with the differences stated in the open. The
alternative is five special cases hidden inside one function pretending to be
uniform, which is precisely how PE got the way it is.

### 4.5 Boot image — `.embboot`, `EMBX_TYPE_BOOT` = 6 — **reserved**

The goal is real: the kernel image itself becomes EMBX, and the existing
two-stage bootloader parses one format instead of a bespoke blob layout. (Note
for the record — EmbLinkOS has never used GRUB; the bootloader is already its
own. What this buys is *one* format across boot and userland, not independence
from GRUB.)

Two constraints this kind imposes on the container, both already honored above:

1. **`paddr` exists because of this kind.** A boot image loads at a physical
   address and runs at a virtual one — the kernel lives at
   `0xFFFFFFFF80100000` but is loaded low by a stage-2 loader running before
   paging. The APP contract does not need `paddr` (the current ELF loader
   correctly ignores `p_paddr`), so it is reserved at segment offset `0x38` and
   MUST be zero for every other kind.
2. **The parse budget is tiny.** A stage-2 loader parses in a constrained
   environment with almost no code space. Hence `segment_table_offset` **should**
   be exactly `128`: header and segment table land contiguously at the front of
   the file, so a minimal loader reads a couple of sectors and has everything it
   needs. `capability_table_offset` points *after* the segments for the same
   reason — a boot image has no grantor and never reads it.

The contract stays reserved until a loader is written for it.

---

## 5. The capability contract

### 5.1 Semantics: contract

A binary declares what it needs. **`spawn()` fails if the grantor cannot satisfy
the declaration.** Not advisory, not intersection.

This matches the refuse-rather-than-misbehave discipline used everywhere else in
this system — EMBKFS refusing an unknown incompat bit, EmbBuild refusing a
self-contradictory manifest, the loader refusing `W|X`. A capability the program
believed it had and silently does not is the same class of bug as a filesystem
mounting with a feature it does not understand.

### 5.2 The invariant this creates

Follow contract semantics down the process tree and it yields a provable
property:

> **No process can hold a capability its parent did not hold.** Capabilities are
> monotonically non-increasing from `init` outward.

That is worth having, and it is exactly the shape of thing a `test caps` case can
exercise directly. It also has a consequence to accept deliberately: once a
capability is dropped along a branch of the tree, nothing below can regain it.
The system therefore needs a designated authority path — `init`, or an explicit
grant service — or it can only *lose* authority over its lifetime. That path is
not defined here; it is the grantor question (§5.4).

### 5.3 A `.embdll` declaration is a requirement on its host

When the linkage contract lands: a shared library's capability table is checked
against the **loading process's** granted set, and the load fails if unsatisfied.
Coherent, and better than the Unix nothing — but written down now so it is not
discovered later.

### 5.4 Grantor — reserved

`grantor` (header `0x48`) **MUST be zero in v1**, and a nonzero value is
`EMBX_EMALFORMED`.

For `.embx` the grantor is implicit and unambiguous: the parent process at
`spawn()`. For `.embmod`, `.embdrv` and `.embboot` there is no parent — the
grantor would be boot policy or an operator, and naming it requires a loader that
does not exist. The field is reserved so that naming it later is additive.

### 5.5 Capability table entry — 16 bytes

| Offset | Size | Field | Notes |
|---|---|---|---|
| `0x00` | 4 | `cap_id` | §5.6 |
| `0x04` | 4 | `cap_flags` | Reserved. MUST be 0 in v1 |
| `0x08` | 8 | `reserved0` | Reserved for a v2 parameter. MUST be 0 |

```c
_Static_assert(sizeof(struct embx_capability) == 16, "EMBX capability entry must be 16 bytes");
```

**The table MUST be sorted ascending by `cap_id`, with no duplicates.** Two
reasons, both structural: the grantor's subset check becomes a single linear
merge instead of a nested scan, and a duplicate or out-of-order entry is a
*parse-time* error rather than a runtime ambiguity about which entry wins. Same
discipline as EmbBuild's parse-time ordering guard.

**There are no optional capabilities in v1.** `cap_flags` is reserved and must be
zero. An optional capability implies the program can discover at runtime whether
it got one, which needs a query syscall that does not exist; without it a program
would have to probe by failing, which is worse than declaring honestly. The bits
are reserved for when that interface is real.

### 5.6 Capability IDs

| ID | Name |
|---|---|
| `0` | Invalid |
| `1` | `EMBX_CAP_FILESYSTEM` |
| `2` | `EMBX_CAP_NETWORK` |
| `3` | `EMBX_CAP_GPU` |
| `4` | `EMBX_CAP_AUDIO` |
| `5` | `EMBX_CAP_CAMERA` |
| `6` | `EMBX_CAP_USB` |
| `7` | `EMBX_CAP_SERIAL` |
| `8` | `EMBX_CAP_RAWDISK` |
| `9` | `EMBX_CAP_KERNEL_EXT` |

`0x100` and above are reserved for **parameterized** capabilities in v2 — a
filesystem capability scoped to a path prefix, a serial capability scoped to one
port. v1 capabilities are coarse and unparameterized because no grantor exists
yet to enforce a parameter, and the 8 reserved bytes per entry are where the
parameter will go.

---

## 6. The load sequence

The algorithm the container is derived from. Each step names its failure code.
**Nothing is mapped until step 9 passes.**

1. Read 128 bytes. Fewer available → `EMBX_ETRUNC`.
2. `magic` mismatch → `EMBX_EMAGIC`.
3. CRC32C over bytes `0x00`–`0x7B` ≠ `header_checksum` → `EMBX_ECHECKSUM`.
   *Nothing in the header is trusted before this step.*
4. `version_major` > known, or `header_size` ≠ 128 → `EMBX_EVERSION`.
5. `feature_incompat & ~EMBX_KNOWN_INCOMPAT` ≠ 0 → `EMBX_EFEATURE`.
6. `binary_type` not `EMBX_TYPE_APP` → `EMBX_ETYPE`. (This is where `.embfw`,
   `.embmod`, `.embdrv`, `.embboot` and — in v1 — `.embdll` are refused.)
7. `machine` ≠ host, or `abi_version` > kernel's supported → `EMBX_EMACHINE` /
   `EMBX_EABI`.
8. `image_size` ≠ actual file size → `EMBX_ETRUNC`. Validate the segment and
   capability tables: every table must lie wholly inside the image, entry sizes
   must match, and every §8 guard must hold → `EMBX_EMALFORMED`.
9. **Capability check.** Read the capability table; verify sorted and unique
   (`EMBX_EMALFORMED`); check every declared capability against the grantor's
   set. Any capability not granted → **`EMBX_ECAPS`**, its own distinct code so
   the shell can report *"denied: declared Network, not granted"* rather than a
   generic spawn failure.
10. Create the address space. For each `LOAD` segment in table order: verify the
    payload's CRC32C (`EMBX_ECHECKSUM`), map `mem_size` bytes at `vaddr` with the
    segment's flags, copy `file_size` bytes, zero-fill the tail.
11. Enter at `entry_point`.

Steps 1–9 are pure validation against a candidate image. Step 10 is the first
irreversible act. That split is the point of the ordering: a rejected binary
costs one page table's worth of nothing.

---

## 7. Error codes

| Code | Meaning |
|---|---|
| `EMBX_EMAGIC` | Not an EMBX image |
| `EMBX_EVERSION` | Format version or header size not understood |
| `EMBX_EFEATURE` | An unknown `feature_incompat` bit is set |
| `EMBX_ETYPE` | This loader does not load this binary type |
| `EMBX_EMACHINE` | Wrong architecture |
| `EMBX_EABI` | Binary requires a newer ABI than the kernel provides |
| `EMBX_ETRUNC` | File shorter than `image_size`, or a table runs past the end |
| `EMBX_ECHECKSUM` | Header or segment checksum mismatch |
| `EMBX_EMALFORMED` | A §8 guard failed |
| `EMBX_ECAPS` | Declared capability not granted |
| `EMBX_ENOMEM` | Out of memory during mapping |

---

## 8. Parse-time guards

Each of these is a **refusal**, never a fixup. Listed together because they are
the spec's real content — a format is defined as much by what it rejects.

**Header**
- `header_size` ≠ 128; `segment_entry_size` ≠ 64; `capability_entry_size` ≠ 16.
- `grantor` ≠ 0; any `reserved*` field ≠ 0.
- `capability_count` > 0 while `capability_table_offset` = 0.

**Segments**
- Any table or segment range extending past `image_size`.
- `mem_size` < `file_size`.
- `align` not a power of two, or < 4096 for `LOAD`.
- `(vaddr - file_offset) mod align` ≠ 0. The congruence rule: without it a
  demand-pager cannot map file pages directly, and the constraint costs a
  producer nothing to honor.
- **`W` and `X` both set on a `LOAD` segment.** W^X is enforced at parse time,
  not at map time.
- `LOAD` segments not sorted ascending by `vaddr`, or overlapping in virtual
  address. Sorting is required so the overlap check is a single linear pass.
- `paddr` ≠ 0 on any kind other than `EMBX_TYPE_BOOT`.
- For APP/DLL: any `vaddr` range reaching `0x0000800000000000` or above.

**Capabilities**
- Table not sorted ascending by `cap_id`, or containing a duplicate.
- `cap_id` = 0, or unknown to this kernel. (An unknown capability is refused, not
  ignored — a grantor cannot honestly grant what it cannot name.)
- `cap_flags` ≠ 0, or `reserved0` ≠ 0.

**Type-specific**
- `EMBX_TYPE_FW` with nonzero `entry_point`, any `X` segment, any nonzero
  `vaddr`, or `capability_count` ≠ 0.
- `EMBX_TYPE_APP` with `entry_point` = 0 or not inside an executable `LOAD`
  segment.

---

## 9. What v1 deliberately does not have

Each with the reason, so the omission is a decision and not an oversight.

| Absent | Reason |
|---|---|
| Compression, encryption | The loader copies eagerly. Both are *loader* features; the incompat bits are reserved so they can arrive without a format break |
| Demand paging, lazy symbol resolution | Same — describe a loader that does not exist |
| Signature block | A signature format without a key-management and trust model is D-003's mistake at a smaller scale. `build_id` gives content identity today; `EMBX_INCOMPAT_SIGNED` is reserved so a v1 loader refuses a signed image rather than ignoring the signature |
| Resource directory | `/data/apps/<name>/` is the resource directory. Embedding icons and fonts in the binary is a Windows answer to a problem EMBKFS does not have |
| Export ordinals | A PE artifact of 1990s DLL versioning |
| Symbol table, exception/unwind tables | Real, but they belong to the linkage contract (§4.2) and to `.embdbg`. Not needed to run a static program |
| `min_kernel_version` | Cut: `abi_version` already answers "will this run here." Two fields for one fact is the same defect as segment-type-vs-flags |
| Timestamp, compiler/linker version strings | Provenance, not load-time — and a timestamp breaks EmbCC's byte-identical stage2 check (§2.5). `build_id` is deterministic provenance |
| Endianness field | §2.3 |
| BSS segment type | `mem_size > file_size` already says it |

**Tooling:** one tool, `embread`, because a format that cannot be dumped cannot
be debugged. `embas`, `embld`, `embnm`, `embstrip`, `embpack`, `embsign`,
`embprof`, `embtrace`, `embfmt`, `embobjdump` arrive when a task demands them —
thirteen tool names before one line of parser is a plan, not a project.

---

## 10. Open questions for v2

1. **The authority path** (§5.2). If capabilities only ever decrease from `init`,
   what grants? This is a kernel design question, not a format question, and it
   blocks the grantor field.
2. **Parameterized capabilities** (§5.6). `FILESYSTEM` scoped to a path prefix is
   the obvious first one; it needs a string table, which the linkage contract
   will bring anyway.
3. **The linkage contract** (§4.2) — the largest remaining piece.
4. **The BOOT contract** (§4.5) — needs a stage-2 parser first.

---

## 11. Changes from the v1 draft

| v1 | v2 | Why |
|---|---|---|
| Endianness header field | **Cut** | §2.3 |
| Timestamp, compiler/linker versions | **Cut** | §2.5, determinism |
| `Minimum Kernel Version` | **Cut** | Redundant with `abi_version` |
| BSS segment type | **Cut** | `mem_size > file_size` |
| Segment *type* enum + protection flags both describing permissions | **Resolved** | One `type` (what to do) + flags (with what permissions); only `LOAD` is mapped |
| Resource directory, export ordinals | **Cut** | §9 |
| Compressed / Encrypted / Lazy Loaded flags | **Bits reserved, no semantics** | §9 |
| 5 binary types, all undefined | **6 types, all reserved by name; contracts for APP and FW only** | §1 |
| 13 companion tools | **1** (`embread`) | §9 |
| Signature block | **Reserved as an incompat bit** | §9 |
| Single "Format Version" | **`version_major/minor` + `feature_incompat`/`feature_compat`** | EMBKFS's proven pattern, adapted: no `ro_compat` analogue exists for a binary |
| Capability list (prose) | **Byte-exact table, sorted + unique, contract semantics, distinct error code** | §5 — the only part of v1 ELF could not already express, and the reason D-003 was reopened |
| No byte offsets anywhere | **Every structure byte-exact with `_Static_assert`** | §2.4 |
