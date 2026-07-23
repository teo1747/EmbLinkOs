# emlibc — Requirements

*The C library native to EmbLinkOS: shaped to the OS, not to POSIX. This is a
**requirements** document — what emlibc must be and must provide — not an
implementation. It is written now, before the code, for the same reason
`docs/BUILD.md` preceded EmbBuild and `EMBX_Specification_v2.md` preceded the
EMBX loader: the reasoning should survive the gap between deciding and building.*

**Status:** requirements, no implementation. Current userland links **newlib**
(rebuilt as newlib-c99); emlibc replaces it *incrementally*, not in a flag day.
**Paired with** EmbCC (the native compiler) and **EMBX** (the native format):
the three are one decision — own the toolchain's output as fully as the kernel
and filesystem are already owned.

---

## 1. The thesis, and what it is not

emlibc is a **non-POSIX** C library. That is the point, and it needs stating
precisely because "non-POSIX libc" sounds like more work than it is.

**A libc is mostly not about the OS at all.** `string.h`, `math.h`, `qsort`,
`malloc`, printf's number formatting — these are pure algorithms with a correct
answer, identical on every operating system. POSIX lives only in the thin
**OS-facing rim**: how I/O, processes, signals, time and entropy are spelled.
So "our own non-POSIX libc" is not "reimplement a libc." It is:

> **own the rim, shaped to EmbLink; lift the OS-agnostic bulk from a permissive
> source; reinvent nothing that already has a correct answer.**

**What emlibc is NOT:**
- Not a POSIX implementation with the gaps stubbed. A function EmbLink has no
  mechanism for is **absent**, not a lying `ENOSYS` stub (THE RULE).
- Not a fork of newlib/musl/glibc. It targets the EmbLink ABI directly.
- Not a from-scratch `cosf`. Owning the transcendentals is authorship without
  capability (EmbCC D-006) — it buys bugs, not identity.
- Not urgent. newlib works and ships; emlibc earns adoption per D-006.

---

## 2. What already exists, and is kept

The rim is **not** a greenfield. EmbLinkOS already owns the parts of a libc that
are actually EmbLink-specific:

| Component | Where | Role emlibc inherits |
|---|---|---|
| `crt0.c` | `user/lib/` | `_start(argc,argv,envp)`, `.init_array`/`.ctors` walk, the weak TLS/ctor bracket symbols — the entry contract |
| `syscalls.c` | `user/lib/` | the retargeting layer: `_read`/`_write`/`_sbrk`/… onto `int 0x80` |
| errno map | `embk_errno_from_kernel()` | kernel numbers → the libc's numbering, mapped by name |
| TLS setup | `newlib.ld` PT_TLS + `set_fs_base` (#58) | thread-pointer install |
| `embk.h` / `embk_syscall.h` | `user/lib/` | the raw EmbLink surface (68 syscalls, typed) |

emlibc's first requirement is therefore **modest**: formalize and own this rim
as emlibc's own, keep it serving the existing static programs, and grow the
agnostic bulk on top over time. The rim is the identity; it is 90% built.

---

## 3. The OS-facing surface emlibc MUST expose

emlibc exposes **what the OS actually does**, in EmbLink's own vocabulary — not
a POSIX costume over it. Concretely it must provide, over the existing syscalls:

- **stdio** (`fopen`/`fread`/`printf`/…) over `open`/`read`/`write`/`close`/
  `lseek`/`stat` — buffered, and it may be lifted; only the retargeting stubs
  underneath are EmbLink-specific. Directory reads over `readdir` (#9) with the
  dirent snapshot the newlib layer already builds.
- **Process** in EmbLink's model: **`spawn` + file-actions**, not `fork`/`exec`.
  A `posix_spawn`-shaped call maps naturally; `fork` is **absent**. `wait`/
  `kill` operate on **handles** (#11/#13), not raw pids.
- **Capabilities** (this OS's, new): `getcaps` (#68), and a helper to build a
  `SET_CAPS` spawn action — a program can declare and inspect its own authority.
- **Time**: `clock_gettime`/`gettimeofday` over HPET/RTC (#19), `sleep` over
  `sleep_ms` (#46). The POSIX-timer machinery newlib needed to self-declare is
  emlibc's to own honestly.
- **Entropy**: `getentropy` = RDRAND or **fail** — never fabricated (existing
  rule, non-negotiable).
- **TLS / threads**: `thread_create`/`thread_join` (#14/#15) as the OS provides;
  the pthreads-over-futex surface is **not** promised, because the kernel is
  spawn+file-actions, not a futex substrate.
- **IPC / UI, optional and EmbLink-native**: channels (#26–#32), surfaces/
  windows (#20–#25, #39–#48) — exposed as themselves, gated by the capability
  model (a program without `GPU` cannot get a surface), not wrapped as sockets
  or ttys.

**Deliberately absent** (not stubbed, not faked): `fork`/`exec`, async signal
delivery to other processes, BSD sockets, `mmap` of arbitrary files, `/proc`.
The kernel has none of these by design; emlibc must not pretend otherwise.

---

## 4. The OS-agnostic bulk emlibc MUST provide (and may LIFT)

Required for real programs, and none of it is EmbLink-specific — so it may be
taken from a permissive-licensed source rather than rewritten, and effort spent
on the rim instead:

- `string.h` / `strings.h`, `ctype.h`, `stdlib.h` (alloc, `qsort`, `strtol`/
  `strtod`, `abs`/`div`), `stdio.h` formatting (the `printf`/`scanf` family,
  including the C99 `%z`/`%ll` newlib had to be rebuilt for — emlibc gets them
  by construction), `math.h` (correctly-rounded transcendentals — **lifted**,
  not authored), `inttypes.h`/`stdint.h`, `assert.h`, `errno.h`, `time.h`
  formatting (`strftime` etc., separate from the clock rim).

The guardrail (D-006, stated once): **spend the scarce hours on the rim's
shape**; a from-scratch `cosf` is the trap.

---

## 5. Build, ABI, and self-hosting requirements

- **ABI-stable rim.** The syscall numbers and `struct spawn_file_action` /
  `struct surface_info` layouts are the contract; emlibc pins them and grows
  them only in lockstep with the kernel (the existing "rebuild every
  action-passing app" discipline).
- **errno is emlibc's own numbering**, mapped from kernel codes by name. A
  libc swap re-opens the errno map — that cost is known and bounded.
- **Deterministic + self-hostable.** emlibc's own source must stay inside the C
  subset EmbCC implements, so that eventually **EmbCC compiles emlibc** — the
  closed loop (compiler + libc + format + loader + OS, one system). Nothing in
  a built emlibc object records wall-clock time (same reason EmbCC's stage2 must
  be byte-identical).
- **EMBX-native output.** A program linked against emlibc ships as an **EMBX**
  binary (`EMBX_Specification_v2.md`): emlibc provides the crt0 that satisfies
  EMBX's `_start(argc,argv,envp)` entry contract, and declares its required
  capabilities in the EMBX capability table. ELF + newlib remains the **porting
  substrate** in parallel (foreign source recompiled), exactly as EMBKFS is
  native while FAT32 reads foreign disks.

---

## 6. Sequencing — incremental, never a flag day

1. **Own the rim as emlibc**: adopt crt0.c + syscalls.c + the errno map under
   the emlibc name; keep every existing static program linking and passing.
2. **Grow the agnostic bulk** header by header (string → stdlib → stdio → math),
   lifting where sensible, so a program can link emlibc instead of newlib for a
   growing subset. newlib stays available the whole time.
3. **Capability-aware surface**: `getcaps`, the SET_CAPS helper, honest process
   model — the things newlib cannot express.
4. **EMBX target**: emit EMBX with a declared capability table.
5. **Self-host**: EmbCC compiles emlibc; adopt per D-006 (a stated capability,
   not authorship).

Each step is testable on its own (a program that links the grown subset and
runs is the proof, the house rule). Adoption is earned, not scheduled.

---

## 7. Where the truth lives

- The syscall surface: `user/lib/embk.h`, `user/lib/embk_syscall.h`; the
  retargeting: `user/lib/syscalls.c`, `user/lib/crt0.c`.
- The native format emlibc targets: `docs/EMBX_Specification_v2.md`.
- The capability model emlibc surfaces: `kernel/process/capabilities.h`,
  `sys_getcaps` (#68), `SPAWN_ACTION_SET_CAPS`.
- The compiler emlibc pairs with, and the ownership/adoption discipline it
  inherits: EmbCC `docs/DECISIONS.md` (D-006 earn-by-capability, D-009 emlibc).
