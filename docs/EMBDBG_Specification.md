# EmbDBG — Debug Format & Kernel Debugging Contract, v1

*The native debugging channel of EmbLinkOS: a byte-exact `.embdbg` debug-info
format **and** the kernel endpoints a debugger drives it through. Two halves of
one contract — the format says what the code *means* (address ↔ source, values,
types), the kernel contract says what a debugger may *do* (stop, inspect, step,
resume) — because neither is useful without the other.*

**Status:** specification, no implementation. Byte layouts and syscall numbers
are reserved here so the reasoning survives the gap between deciding and
building — the discipline `EMBX_Specification_v2.md` and `EMLIBC_Requirements.md`
were written under. Nothing in the kernel references debug state yet
(`grep -r ptrace kernel/` is empty, on purpose — this is the design that fills
that void).

**Relationship to EmbCC's `docs/EMBDBG_Requirements.md` (D-010).** That document
governs the *producer* side and its ordering, and this spec does **not** revise
it. D-010 says: **DWARF is the host bridge** (EmbCC emits `.debug_line` so `gdb`
can debug EmbCC output today, on the host, against a consumer that already
exists), and the **native `.embdbg` is derived last, from what an EmbLinkOS
debugger actually needs — which needs the kernel debugging contract to exist
first** (D-010 §8 Q1, explicitly flagged as a *kernel* design question, out of
EmbCC's scope per D-007). This document is that missing substrate. Designing the
kernel contract, and deriving the format from the queries it makes real, *is*
following D-010 — not jumping it. DWARF-on-the-host and `.embdbg`-on-EmbLinkOS
are the same dual-form move EMBX makes: ELF as the porting lane, the native
format for the owned world.

**Consumes / pairs with:** EMBX (`EMBX_Specification_v2.md` — carries the
`build_id` this format binds to, and the `EMBX_SEG_DEBUG` / `DEBUG_SIDECAR`
slots it lives in), the capability model (`kernel/process/capabilities.h`), the
kernel crypto already present (CRC32C + SHA-256, from EMBKFS Phase 2), and the
existing exception dump (`kernel/arch/x86_64/irq/isr.c`) it upgrades.

---

## 1. Design rules (inherited)

The same rules `EMBX_Specification_v2.md` §2 states, because this is the same
kind of artifact and the same OS:

1. **Derive the shape from the queries, container last.** The byte layout of
   every section below is dictated by exactly one lookup a debugger performs.
   No field exists that no query reads.
2. **Byte-exact, one reader.** Fixed-width, aligned, little-endian records.
   Where a choice exists between a compact encoding with a decode path and a
   larger one that is directly seekable, prefer the seekable one — a decode path
   that runs on every lookup is a cost the EMBKFS/EMBX aesthetic refuses (EMBX
   §9: "no decode path that never runs"). The line table is a binary-searchable
   array, not a bytecode state machine.
3. **Deterministic.** No timestamps, no absolute host paths, tables sorted by a
   defined key. Identical input → byte-identical `.embdbg`. This is the same
   rule that lets EmbCC's stage2 be byte-identical (EmbCC DECISIONS §2.5), and
   debug output must not break it (D-010 §5).
4. **Strippable, absence lossless.** A program runs identically with the debug
   channel gone. EMBX already encodes this: the info is a **sidecar**,
   `EMBX_F_STRIPPED` says it is absent, `EMBX_COMPAT_DEBUG_SIDECAR` says it is
   present — a *compat* bit, safe for any loader to ignore.
5. **Reserve by name, define on use.** Kinds and flags with no consumer yet are
   listed and numbered so the format grows without renumbering, but carry no
   byte layout until something reads them (INLINE, MACRO, full CFI — §5.9).
6. **Refuse loudly; never fake.** The format can *say* "not available here"
   (`LOC_NONE`, §5.7) rather than point at a stale slot; the kernel contract
   returns a distinct error rather than a plausible-wrong success. THE RULE,
   applied to debugging: a debugger that confidently shows a wrong value is
   worse than one that admits it cannot see.
7. **Prove on the host first, then on the OS** (EmbCC D-005). The line/frame
   tables are checkable against `addr2line`/`gdb` on the host before the kernel
   ever reads one.

---

## 2. Two gifts that make this smaller than DWARF

The design leans on two properties of *this* toolchain, and both shrink the
format relative to a general-purpose one:

- **EMBX APPs are fully linked — no relocations** (EMBX §4.1). Link-time virtual
  address equals run-time virtual address; there is no load slide. Every address
  in `.embdbg` is an **absolute vaddr**, needing no relocation and no
  per-process rebasing. (The kernel's own image is fixed in the higher half for
  the same reason.) DWARF carries machinery for position-independent, relocated,
  and split-DWARF images; none of it is needed here.

- **EmbCC keeps `rbp` as a frame pointer** (D-010 §8 Q2). Unwinding is a pointer
  walk — `caller_rbp = *rbp`, `return_addr = *(rbp+8)` — so the frame section
  (§5.5) is a tiny per-function descriptor recording only the prologue/epilogue
  boundaries where `rbp` is not yet / no longer the frame base. DWARF's
  Turing-complete CFI expression stack is reserved (§5.9) for the day codegen
  optimizes `rbp` away, and only then.

These are not shortcuts that cost correctness; they are the format being exactly
as large as EmbLinkOS's debugging needs and no larger.

---

## 3. Where `.embdbg` lives, and what binds it to its binary

Two carriers, one format:

- **Sidecar (default).** `foo.embx` ships with `foo.embdbg` beside it. The EMBX
  image sets `EMBX_COMPAT_DEBUG_SIDECAR` (`0x1`, §3.6) so a reader knows to look;
  stripping is `rm foo.embdbg` and setting `EMBX_F_STRIPPED`. The shipped binary
  stays small.
- **Embedded segment.** The same bytes may instead be an `EMBX_SEG_DEBUG`
  segment (kind 5, not mapped, §3.3) for single-file distribution. The header
  flag `DBG_F_EMBEDDED` marks this so a reader knows the offsets are
  segment-relative.

**The binding — and the modern property that falls out of it.** The `.embdbg`
header repeats the owning image's `build_id` (SHA-256, the exact 32 bytes at
EMBX offset `0x50`). A debugger — and the kernel symbolizer — **verify the
build_id matches the running image before trusting a single offset.** Stale
debug info, the classic footgun where `gdb` shows lines from a binary you
rebuilt, is *structurally impossible*: mismatched build_id is a hard refusal
(`EMBDBG_EBUILDID`), not a silent wrong answer. EMBX already computes this hash;
EmbDBG spends nothing to inherit it.

The kernel's **own** debug info (for symbolizing panics, §7) is a `.embdbg`
carried in the boot image, its `build_id` the kernel image's hash, flagged
`DBG_F_KERNEL` (addresses are higher-half kernel vaddrs).

---

## 4. Container: header + section table

Little-endian throughout. All offsets are from the start of the `.embdbg` (or,
when `DBG_F_EMBEDDED`, from the start of the `EMBX_SEG_DEBUG` segment).

### 4.1 Header — 64 bytes, fixed

| Offset | Size | Field | Notes |
|---|---|---|---|
| `0x00` | 8 | `magic` | `7F 45 4D 44 42 47 0A 1A` — `\x7F` `EMDBG` `\n` `\x1A`. Mirrors EMBX's `\x7F…\x1A` DOS-EOF trap byte. |
| `0x08` | 2 | `format_version` | `1` |
| `0x0A` | 2 | `header_size` | `64` for v1; a reader trusts `header_size`, not the constant, so a v2 may grow the header |
| `0x0C` | 4 | `flags` | §4.3 |
| `0x10` | 32 | `build_id` | SHA-256 of the owning image — **must** equal EMBX `0x50` (§3) |
| `0x30` | 2 | `section_count` | |
| `0x32` | 2 | `reserved` | `0` |
| `0x34` | 4 | `section_table_offset` | to the first section-table entry |
| `0x38` | 4 | `file_size` | total `.embdbg` bytes (segment bytes when embedded) |
| `0x3C` | 4 | `header_checksum` | CRC32C over `0x00`–`0x3B`, this field taken as zero — validates the header **before** any offset inside it is trusted, exactly as EMBX §3.4 does |

### 4.2 Section-table entry — 24 bytes

| Offset | Size | Field | Notes |
|---|---|---|---|
| `0x00` | 2 | `kind` | §5 |
| `0x02` | 2 | `flags` | per-kind; `0` in v1 |
| `0x04` | 4 | `entsize` | bytes per record; `0` for a variable-length blob (STRTAB, TYPES) |
| `0x08` | 4 | `count` | record count; `0` for a blob |
| `0x0C` | 4 | `offset` | to the section body |
| `0x10` | 4 | `size` | section body bytes |
| `0x14` | 4 | `checksum` | CRC32C over the section body |

A `kind` a reader does not understand is **skipped** (its `offset`/`size` make it
skippable) — the reserve-by-name rule (§5.9) works because unknown sections cost
nothing.

### 4.3 Header flags

| Bit | Name | Meaning |
|---|---|---|
| `0x1` | `DBG_F_EMBEDDED` | info is an `EMBX_SEG_DEBUG` segment; offsets are segment-relative |
| `0x2` | `DBG_F_KERNEL` | kernel's own debug info; addresses are higher-half vaddrs |
| `0x4` | `DBG_F_SOURCE_HASHED` | FILES entries carry source content hashes (§5.2) |
| `0x8` | `DBG_F_OPT` | producer optimized; `LOC_NONE` entries may appear (§5.7) — a reader must be ready to say "optimized out" |

---

## 5. Sections

Each section answers exactly one query. The queries, in D-010's ascending-cost
order (§1 of that doc): **where am I** (5.1–5.5), **what can I see** (5.6),
**what is it** (5.8).

### 5.1 STRTAB (kind 1) — blob

NUL-terminated UTF-8 strings; byte offset `0` is the empty string. Every `*_str`
field elsewhere is a u32 offset into this blob. Deterministic ordering: strings
appear in first-reference order.

### 5.2 FILES (kind 2) — `entsize 40`

The source files. Row:

| Off | Size | Field |
|---|---|---|
| `0x00` | 4 | `path_str` — file name (STRTAB offset) |
| `0x04` | 4 | `dir_str` — directory (STRTAB offset), project-relative for determinism |
| `0x08` | 32 | `content_hash` — SHA-256 of the source text, or zero unless `DBG_F_SOURCE_HASHED` |

`content_hash` lets a debugger warn **"source has changed since this was
compiled"** instead of showing lines that no longer match — the same
stale-info-is-impossible property as `build_id`, one level down. Paths are
project-relative so the `.embdbg` is byte-identical regardless of the build
directory (§1 rule 3).

### 5.3 LINE (kind 3) — `entsize 16`, sorted ascending by `addr`

The address↔source map. The single most valuable section (D-010 §1). Row:

| Off | Size | Field |
|---|---|---|
| `0x00` | 8 | `addr` — absolute vaddr of the first instruction of this row |
| `0x08` | 4 | `line` — 1-based source line; `0` = "no source here" (end of a function's code, padding) |
| `0x0C` | 2 | `file` — index into FILES |
| `0x0E` | 2 | `col_flags` — column in low 12 bits (0 = unknown); flags in high 4 bits |

`col_flags` high bits: `0x1000` `LN_STMT` (recommended breakpoint line),
`0x2000` `LN_PROLOGUE_END` (first instruction past the prologue — where a
function breakpoint should land so arguments are live), `0x4000` `LN_EPILOGUE`.

**Query — PC → source:** binary-search for the greatest `addr ≤ PC`; that row's
`file:line:col` is the location. **Query — line → PC** (breakpoint by line):
scan for the row with matching `file`/`line` and lowest `addr`, preferring an
`LN_STMT` row. A fixed 16-byte binary-searchable array, no state machine — the
seekable-over-compact choice of §1 rule 2. (Delta-encoding is reserved as a v2
`kind` if image size ever demands it; it does not today.)

### 5.4 FUNCS (kind 4) — `entsize 32`, sorted ascending by `low_pc`

| Off | Size | Field |
|---|---|---|
| `0x00` | 8 | `low_pc` — first vaddr |
| `0x08` | 8 | `high_pc` — one past the last vaddr (half-open) |
| `0x10` | 4 | `name_str` |
| `0x14` | 4 | `frame_idx` — index into FRAME (§5.5) |
| `0x18` | 4 | `first_var` — index into VARS of this function's first entry |
| `0x1C` | 4 | `var_count` |

**Query — PC → function:** binary-search `low_pc`, confirm `PC < high_pc`. The
name feeds every backtrace frame. (The EMBX/ELF `.symtab` already gives coarse
`STT_FUNC` attribution — D-010 §2 — so FUNCS is not the *only* source of a name,
but it is the one keyed to the frame and variable tables.)

### 5.5 FRAME (kind 5) — `entsize 16`

How to walk from a frame to its caller. One descriptor per function. Because
`rbp` is a frame pointer (§2), this is small:

| Off | Size | Field |
|---|---|---|
| `0x00` | 4 | `prologue_end_off` — offset from `low_pc` where `rbp` becomes the frame base |
| `0x04` | 4 | `epilogue_start_off` — offset from `low_pc` where `rbp` is restored |
| `0x08` | 1 | `kind` — `FRAME_RBP` (0, standard chain) or `FRAME_RSP` (1, leaf, CFA = rsp+`cfa_off`) |
| `0x09` | 1 | `flags` |
| `0x0A` | 2 | `cfa_off` — for `FRAME_RSP`, CFA = `rsp + cfa_off`; unused for `FRAME_RBP` |
| `0x0C` | 4 | `reserved` |

**Query — unwind at PC** (with `func` from §5.4, registers from the kernel):
- `PC < low_pc + prologue_end_off` (in the prologue): `rbp` not yet set →
  CFA = `rsp + 8`, return address at `*rsp`.
- `PC ≥ low_pc + epilogue_start_off` (in the epilogue): `rbp` already restored →
  same as prologue.
- otherwise (`FRAME_RBP`, the common body): CFA = `rbp + 16`, caller `rbp` at
  `*rbp`, return address at `*(rbp+8)`.

Repeat with the recovered `(rbp, PC=return_addr)` until `rbp` leaves the mapped
stack or a return address leaves any FUNCS range. This is the entire unwinder;
the full-CFI `kind` (§5.9) exists only for a future optimizing codegen.

### 5.6 VARS (kind 6) — `entsize 40`

Parameters and locals — "what can I see" (D-010 §1). Referenced by range from
FUNCS. Row:

| Off | Size | Field |
|---|---|---|
| `0x00` | 4 | `name_str` |
| `0x04` | 4 | `type_off` — byte offset into TYPES (§5.8); `0` = void/unknown |
| `0x08` | 8 | `scope_lo` — vaddr where the variable becomes live |
| `0x10` | 8 | `scope_hi` — vaddr where it dies (lexical block bounds; = function bounds for top-level locals) |
| `0x18` | 1 | `loc_kind` — §5.7 |
| `0x19` | 1 | `var_flags` — `0x1` `VAR_PARAM` |
| `0x1A` | 6 | `reserved` |
| `0x20` | 8 | `loc` — interpreted per `loc_kind` |

**Query — locals in scope at PC:** over the function's VARS rows, take those with
`scope_lo ≤ PC < scope_hi`; resolve each via `loc_kind`/`loc`; render via TYPES.

### 5.7 Location kinds (`loc_kind`)

| Value | Name | `loc` means |
|---|---|---|
| 0 | `LOC_NONE` | **not available at this PC** — optimized out, or not yet live. The honest state (§1 rule 6). v1 codegen never emits it; reserved so post-M4 optimization is *expressible* rather than lying with a stale slot (D-010 §5). |
| 1 | `LOC_FBREG` | at `[rbp + (int64)loc]` — the uniform case for EmbCC's every-value-in-a-slot codegen (D-010 §4 step 2) |
| 2 | `LOC_REG` | in register number `loc` (DWARF x86-64 numbering: 0=rax … 6=rbp … 15=r15) |
| 3 | `LOC_ABS` | at absolute vaddr `loc` (a global/static; pairs with §5.4 for file-scope objects) |
| 4 | `LOC_REGADDR` | address is in register `loc` (a value passed/held by pointer) |

### 5.8 TYPES (kind 7) — blob, offset-referenced

A flat tagged blob; a `type_off` is a byte offset into it. Offset `0` is
reserved for `void`. Each entry begins with a `u8 tag`; recursion is by
`type_off` reference, so the graph is expressed without pointers. This is DWARF's
type DIEs, flattened.

| Tag | Name | Body |
|---|---|---|
| 1 | `T_BASE` | `u8 encoding` (1 signed, 2 unsigned, 3 float, 4 bool, 5 char, 6 uchar), `u8 size`, `u32 name_str` |
| 2 | `T_POINTER` | `u32 target_off` |
| 3 | `T_ARRAY` | `u32 elem_off`, `u32 count` (`0xFFFFFFFF` = unknown/flexible) |
| 4 | `T_STRUCT` | `u32 name_str`, `u32 byte_size`, `u16 member_count`, then `member_count` × `{u32 name_str, u32 type_off, u32 byte_offset}` |
| 5 | `T_UNION` | as `T_STRUCT`, all members at `byte_offset 0` |
| 6 | `T_ENUM` | `u32 name_str`, `u32 underlying_off`, `u16 count`, then `count` × `{u32 name_str, i64 value}` |
| 7 | `T_TYPEDEF` | `u32 name_str`, `u32 target_off` |
| 8 | `T_QUAL` | `u8 qual` (bit0 const, bit1 volatile), `u32 target_off` |
| 9 | `T_FUNC` | `u32 ret_off`, `u16 param_count`, then `param_count` × `u32 type_off` |

Member `byte_offset`s are exactly the SysV-layout offsets EmbCC's
struct-classification work already computes (D-010 §4 step 3) — the type graph is
*complete at compile time and thrown away* today; this section is where it is
kept. Bitfields are reserved (a `T_STRUCT` member flag) until the frontend has
them.

### 5.9 Reserved kinds and locations (named, no layout — §1 rule 5)

- **kind 8 `INLINE`** — inlined-frame ranges, for backtraces through inlining.
  No consumer until codegen inlines (post-M4).
- **kind 9 `MACRO`** — macro definitions, for expression evaluation in macro
  scope. Rarely worth its size; named so it is not renumbered into.
- **kind 10 `CALLFRAME`** — full CFI (DWARF-style register rules) for when
  `rbp`-frames stop holding. The §5.5 descriptor is the v1 subset; this is its
  superset, and neither exists until an optimizing codegen does (D-010 §5,
  "optimization-honest").

---

## 6. The kernel debugging contract

The format above is inert without a way to stop a program and look. This is that
mechanism — D-010 §8 Q1's "kernel debugging contract," designed in EmbLinkOS's
own model: **typed handles, capability-gated, no ambient authority, no `ptrace`,
no signals-to-others.** It is a *design*, reserved here; §8 stages its build.

### 6.1 Authority — a debugger is attenuated, not omnipotent

Debugging reads and writes another process's registers and memory: it is
**strictly more powerful than any resource capability**, so it is gated by its
own, new capability class:

- **`EMBK_CAP_DEBUG` = cap_id 10** (bit 10). `EMBK_CAP_MAX_ID` becomes `10`;
  `EMBK_CAP_ALL` grows to bits 1..10 (held by `init` and kernel threads, §
  `capabilities.h`). It attenuates down the process tree like every other
  capability (`embk_caps_attenuate`) — a sandbox that spawns children *without*
  `CAP_DEBUG` produces processes that can neither debug nor host a debugger,
  with **zero new enforcement code**: it is the existing monotonic invariant.

- **You may debug process P only if** you hold `CAP_DEBUG` **and** you hold a
  handle to P. Spawn returns handles (not pids), so by default a process may
  debug the descendants it spawned and nothing else. There is no
  "attach to any pid" — that would be the ambient authority the whole model
  rejects. This is the security property `ptrace` needed YAMA/`ptrace_scope` to
  bolt on afterward; here it is native and derives from the capability graph.

### 6.2 Attach

Two ways to get a **debug handle** (a new typed object handle; operations below
take it):

- **Born under debug (preferred).** A new spawn file-action
  **`SPAWN_ACTION_DEBUG` (id 6)** spawns the child **stopped before `_start`**
  and yields a debug handle. This is the clean capability flow: the parent
  already has authority over a child it is creating, so it needs only
  `CAP_DEBUG`. The debugger sets breakpoints on the stopped image, then continues
  it into `_start`.

- **Attach to a running descendant.** `sys_debug_attach(handle) → dbg_handle`
  (§6.4) stops an already-running process the caller holds a handle to. Same two
  gates (`CAP_DEBUG` + handle).

### 6.3 The stop/inspect/resume loop

The heart of the contract, in EmbLinkOS's blocking-syscall style (a debugger is
an ordinary process that blocks in `sys_debug_wait`, exactly as a reader blocks
in `read`):

1. Target hits a **stop condition** — a planted `INT3`, a single-step, a fault,
   or exit.
2. The kernel **parks the target's threads** and records a stop event.
3. The debugger's `sys_debug_wait` returns that event.
4. The debugger reads/writes registers (`sys_debug_regs`) and memory
   (`sys_debug_mem`), consulting `.embdbg` to turn addresses into lines and bytes
   into typed values.
5. The debugger **resumes** with `sys_debug_cont` (continue or single-step).

**Software breakpoints are debugger-side, not a syscall.** The debugger plants
`0xCC` with `sys_debug_mem` and keeps the saved byte itself — exactly how `gdb`
does it, and why the kernel needs no breakpoint table. The kernel's only job is
to **deliver `#BP` as a stop event instead of a panic**, and to single-step over
the restored instruction on resume (the standard lift-step-replant dance, done
by the debugger). This keeps the kernel surface minimal and honest: the kernel
provides the *primitives* (stop delivery, register access, memory access,
single-step, hardware breakpoints), the debugger composes the *policy*
(breakpoints, conditions, step-over, step-into).

### 6.4 Endpoints (reserved syscall numbers 69–75)

| # | Name | Signature | Meaning |
|---|---|---|---|
| — | `SPAWN_ACTION_DEBUG` | file-action id 6 | spawn child stopped at entry; a debug handle is returned via the spawn result |
| 69 | `sys_debug_attach` | `(handle) → dbg` | stop a running descendant; needs `CAP_DEBUG` + handle |
| 70 | `sys_debug_wait` | `(dbg, &event, flags) → tid` | block until the target stops; fill `embk_debug_event` (§6.5) |
| 71 | `sys_debug_cont` | `(dbg, tid, action, data) → 0` | resume: `DBG_CONT` (run) or `DBG_STEP` (set TF, one instruction). `tid = 0` = all threads |
| 72 | `sys_debug_regs` | `(dbg, tid, buf, len, write) → n` | read/write a stopped thread's saved GP registers (`struct embk_debug_regs`, the `struct regs` layout) |
| 73 | `sys_debug_mem` | `(dbg, addr, buf, len, write) → n` | cross-address-space read/write of target memory — plants breakpoints, reads variables |
| 74 | `sys_debug_hwbp` | `(dbg, tid, slot, addr, kind, len, enable) → 0` | hardware breakpoint/**watchpoint** via `DR0-3`/`DR7`. `kind` = `HW_EXEC`/`HW_WRITE`/`HW_RDWR`. The only way to break on *data* change |
| 75 | `sys_debug_detach` | `(dbg, disp) → 0` | remove all injected state; `disp` = `DETACH_RUN` or `DETACH_KILL` |

`sys_debug_mem` crosses address spaces: it maps through the **target's** `pml4`,
and must capture that `pml4` with interrupts off — the exact per-CPU migration
race that dropped a `sys_read` byte (`sys-read-silent-byte-drop` in the tree's
ledger) applies identically here and its fix is the precedent.

### 6.5 The stop event — `embk_debug_event`

```c
struct embk_debug_event {
    uint32_t tid;          /* which thread stopped */
    uint32_t reason;       /* DBG_EV_* below */
    uint64_t pc;           /* RIP at the stop */
    uint64_t fault_addr;   /* CR2 for a page fault, else 0 */
    uint32_t vector;       /* CPU exception vector, or 0 */
    uint32_t error_code;   /* CPU error code, or exit status for EXITED */
    uint64_t hwbp_hit;     /* bitmask of DRx that fired, else 0 */
};
```

`reason` values: `DBG_EV_BREAKPOINT` (`#BP`), `DBG_EV_STEP` (`#DB` from TF),
`DBG_EV_WATCHPOINT` (`#DB` from a DRx), `DBG_EV_FAULT` (any other exception — the
program crashed *into the debugger*, which is how "which line faulted" is
answered, D-010 §1), `DBG_EV_THREAD_CREATE`, `DBG_EV_THREAD_EXIT`,
`DBG_EV_EXITED`. A modern debugger wants thread lifecycle events, so they are in
the contract from the start, not retrofitted.

### 6.6 Exception routing — the kernel-internal half

This is the substantive kernel change the contract requires. Today
`isr_handler` (`kernel/arch/x86_64/irq/isr.c`) treats **every** exception as
fatal: it takes `panic_lock` and halts the machine. The debug contract inserts a
pre-dispatch *before* that path, keyed on a new `process->debug_session` pointer:

- **Fault from a thread whose process has a debug session** → do **not** panic.
  Snapshot the register frame (already on the kernel stack as `struct regs`),
  park the faulting thread on the session's wait-queue with the matching
  `DBG_EV_*` reason, and wake the debugger's `sys_debug_wait`. On
  `sys_debug_cont` the frame is restored (with TF set in `eflags` for
  `DBG_STEP`) and the thread `iretq`s back to userspace normally.
- Intercepted vectors: `1` (`#DB`), `3` (`#BP`), and the fault vectors `0`, `6`,
  `13`, `14` — so a debugger catches crashes, not just planted breakpoints.

**A related honesty note, sequenced alongside.** For a **non**-debugged
*userspace* fault, the correct behaviour is to **terminate that process** and
keep the kernel alive — not halt the machine, which is what `isr_handler` does
today. The debug pre-dispatch is the natural site to add that
"user fault → kill the process" arm. It is a real behaviour change to the fault
path and is called out here (not smuggled in) so it is built and tested
deliberately, with the kernel-internal `#DB`/`#BP` uses (single-step of
`sys_read` retries, etc.) audited first — the fault path is load-bearing and
delicate (see the `if-leak-voluntary-block` and SMP-scheduler ledgers).

---

## 7. Two consumers of one format, from day one

The format earns **exactly one on-disk shape** serving two real, in-tree readers
— honoring D-010 §5's "one reader" against the failure mode of a speculative
tool suite (EMBX §9: "thirteen tool names before one parser is a plan, not a
project"). Both readers use the same LINE + FUNCS + FRAME core:

- **EmbDBG** — the userspace source-level debugger. Drives §6's endpoints; reads
  the *target's* `.embdbg` to break by `file:line`, print typed locals, walk a
  symbolized backtrace. This is the "most modern debugger" the effort aims at,
  and it is a userspace app gated by `CAP_DEBUG`.

- **The kernel panic symbolizer** — the kernel loads its **own** `.embdbg`
  (`DBG_F_KERNEL`) at boot and upgrades `isr_handler`'s dump from
  `RIP: 0xffffffff801234ab` to `RIP: vmm_map_in at vmm.c:214`, plus an
  `rbp`-chain backtrace with a source line per frame. This needs **no debugger,
  no userspace, and no producer beyond a LINE+FUNCS+FRAME table** — which is why
  it is the most self-contained first win, and why it is §8's M2. It reuses the
  *exact* unwinder of §5.5.

The second consumer is why the format is designed for a symbolizer, not only an
interactive debugger: turning a crash dump into `file:line` + a backtrace is a
large fraction of real kernel debugging, and it is reachable long before EmbDBG
exists.

---

## 8. Staging — each stage a test that exercises the invariant

The house rule (EmbCC CONTRIBUTING, EMBKFS discipline): a thing is done when a
test exercises the invariant, not when it compiles. Ordered by what each proves,
smallest real thing first.

**M1 — the kernel contract, proven with a raw breakpoint (no format, no
producer).** A witness (`test debug`): a parent with `CAP_DEBUG` spawns a child
under `SPAWN_ACTION_DEBUG`, plants `0xCC` at a known address with
`sys_debug_mem`, continues; the child hits it; `sys_debug_wait` returns
`DBG_EV_BREAKPOINT` at that PC; `sys_debug_regs` shows the expected state;
`sys_debug_mem` reads a known variable; `sys_debug_cont` resumes; the child
exits with the expected code. Reports pass/fail via exit code. This proves
attach/wait/regs/mem/cont/detach **and** the §6.6 exception routing — the gate
for everything else. Needs *no* `.embdbg` at all.

**M2 — the format + the kernel panic symbolizer.** Load the kernel's own
`.embdbg`; `test debugsym` provokes a controlled fault and asserts the dump names
the right `func` + `file:line` and that the `rbp`-chain backtrace matches a known
call path. Needs a LINE+FUNCS+FRAME producer — a **host converter from the
kernel's DWARF** (`addr2line` is the oracle) or hand-authored — but **no**
userspace debugger and **no** EmbCC change. First real bytes of the format,
against the cheapest real consumer.

**M3 — source-level userspace debugging.** EmbDBG (the userspace app) + a
producer of app `.embdbg` — either EmbCC emitting it, or the DWARF→`.embdbg`
bridge (D-010's DWARF line info converted). Acceptance: break by `file:line` in
an EmbCC-compiled program on the OS, print a typed local matching a gcc-built
program's rendering — the same cross-check EmbBuild, TCC and EmbCC each faced.
This is where VARS + TYPES earn out, and it depends on EmbCC's D-010 steps 1–3.

**Reserved futures (§1 rule 5 — named, not promised).** Non-stop mode (per-thread
stop/go), conditional/count breakpoints (debugger-side, no kernel change),
tracepoints, and **reverse / time-travel debugging** — which needs record-replay
and is honestly expensive; named so the format's determinism and the contract's
event model are built compatibly with it, not so it is on a schedule. A native
**serial kernel stub** (a `kgdb` equivalent speaking this contract for on-hardware
kernel debugging) complements — does not replace — today's QEMU `gdbstub` path
(`docs/GDB_CHEATSHEET.md`), which remains the way the kernel is debugged under
emulation right now.

---

## 9. Where the truth lives

- The format's binding and carriers: `docs/EMBX_Specification_v2.md` §3.3
  (`EMBX_SEG_DEBUG`), §3.6 (`EMBX_COMPAT_DEBUG_SIDECAR`, `EMBX_F_STRIPPED`), §3.4
  (`build_id`, CRC32C).
- The producer side and its ordering (DWARF-first, `.embdbg`-derived-last):
  EmbCC `docs/EMBDBG_Requirements.md` (D-010). **This spec does not revise it.**
- The capability the contract adds: `kernel/process/capabilities.h`
  (`EMBK_CAP_DEBUG`, cap_id 10 — reserved here, not yet added).
- The exception path the contract upgrades: `kernel/arch/x86_64/irq/isr.c`
  (`isr_handler`), and the register frame it reads: `struct regs` in
  `kernel/arch/x86_64/syscall/syscall.h`.
- Today's kernel debugging, until the native stub exists:
  `docs/GDB_CHEATSHEET.md` (QEMU `gdbstub`, monitor `info registers`).
- The crypto the format reuses: CRC32C + SHA-256 in `kernel/crypto/` (EMBKFS
  Phase 2).
```
