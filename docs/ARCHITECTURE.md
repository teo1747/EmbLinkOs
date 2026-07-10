# EmbLinkOS — Architecture

*Living document — last updated 2026-07-05.*

**Status legend:**
`✅ Built` — implemented and verified · `🎯 Designed` — decided, not yet implemented · `🔓 Open` — undecided fork · `⏳ Later` — decided direction, off the near-term critical path

> Implementation status below is reconstructed for the design record. `PROJECT_STATUS.md` and the repo are the ground truth for what is actually built.

---

## 1. Identity & Design Philosophy

EmbLinkOS is an x86_64 operating system built from absolute zero, with its own personality — its own kernel model, syscall ABI, security model, shell, tooling, and eventually its own editor. It is **not** a Linux clone and does **not** aim to run all Linux software. It takes inspiration from Unix, BSD, and modern systems, but adopts their designs only where they are genuinely the best answer.

**The governing principle** (emerged consistently across every decision below):

> **Bless the clean native primitive; provide the compatible one as an opt-in layer.**
> Diverge from Unix only with a concrete technical justification — better performance, stronger isolation, a cleaner API, easier maintenance, or better developer experience. Where the Unix way is genuinely best, keep it. Identity is cheap in the parts that are ours anyway (kernel internals, syscall surface, tooling, UI); identity is expensive where the ecosystem tax is savage (C ABI, ELF, calling convention). Be novel where it's cheap; be conservative where the tax is high.

This is why several decisions here **keep** Unix (files as descriptors, uid/gid/mode on disk) while others **diverge** (message ports over signals, `spawn()` over `fork()`, typed handles for structural objects). The distinction — not "always diverge" — is the actual soul of the design.

**Definition of "understand":** every line of the *contract* EmbLinkOS presents (syscall ABI, libc surface, ELF loader, process model, filesystem) is understood completely. Large ported dependencies (a C compiler, later a full libc) are integrated deliberately — we understand *what they do as a whole and how to use them, and why they need what they ask of us*, not every internal line. The understanding lives at the seam.

**North star — self-hosting:** a compiler running on the metal, so the OS can be developed from inside itself. Honest scope: the near-term target is **TCC compiling a real program on bare metal** (C, single self-contained binary, unoptimized) — legitimate self-hosting, and a realistic stretch goal. Full GCC/C++ self-hosting is a later, multi-stage effort, not an end-of-2026 goal for a solo hand-built project.

---

## 2. The Stack

| # | Layer | Component | Status |
|---|-------|-----------|--------|
| 1 | Boot / firmware | Two-stage BIOS bootloader (LBA via INT 13h ext.), E820 map, ELF kernel loader → higher half | ✅ Built |
| | | UEFI bootloader | ⏳ Later |
| 2 | Kernel core (ring 0) | GDT/IDT/TSS, exception handlers | ✅ Built |
| | | APIC (8259 PIC retired), ACPI/MADT | ✅ Built |
| | | Bitmap PMM | ✅ Built |
| | | Higher-half VMM (kernel @ `0xFFFFFFFF80100000`) | ✅ Built |
| | | Buddy allocator (PMM upgrade) | ⏳ Later |
| | | Per-process address spaces | ✅ Built *(`mmap`/`brk` still 🎯 Designed)* |
| | | Copy-on-write pages, NX bit | ⏳ Later *(COW arrives with `fork()`)* |
| | | Scheduler — see [`architecture/process-and-scheduling.md`](architecture/process-and-scheduling.md) | 🚧 Built (timer-preemptive priority-band round-robin + aging, wait queues, real blocking wait; no SMP yet — see spec §17) |
| | | Ring 3 entry (`int 0x80`/`iretq`) | ✅ Built *(`SYSCALL`/`SYSRET` fast path still 🎯 Designed)* |
| 3 | Process & IPC | Process objects, context switch — see [`architecture/process-and-scheduling.md`](architecture/process-and-scheduling.md) | 🚧 Built (no thread/process split yet — see spec §4.1); per-process fd tables ✅, parent/child tracking ✅ |
| | | `spawn()`-shaped process creation (`process_create`, ELF-based) | 🚧 Built (`sys_spawn`/`sys_wait`/`sys_kill` ✅, capability handles not raw pids ✅ — see spec §15.2; still no file-actions list) |
| | | Uncatchable kernel-level kill | ✅ Built (`process_kill`; exposed to ring 3 via `sys_kill`, capability-handle-gated) |
| | | Message/event ports | 🎯 Designed |
| | | Pipes | 🎯 Designed |
| 4 | Driver model | ACPI enumeration | ✅ Built |
| | | PCI/PCIe enumeration (legacy CAM) | ✅ Built *(ECAM/MCFG still 🎯)* |
| | | AHCI storage (LBA48 r/w) | ✅ Built |
| | | USB (xHCI/EHCI/UHCI/OHCI: HID keyboard, mass storage, hub support on legacy HCs) | ✅ Built *(xHCI hub support, isochronous transfers still 🎯)* |
| | | NVMe, PS/2 controller init, RTC | ⏳ Later / as needed |
| | | VBE framebuffer (+ Bochs DISPI / VirtIO-GPU accelerated path, EDID + mode enumeration) | ✅ Built |
| 5 | Block & filesystem | Block layer + DMA bounce buffers | ✅ Built |
| | | VFS (mount registry, `vfs_resolve`, `vget`, `ls`) | ✅ Built |
| | | EMBKFS (native CoW B-tree FS) | ✅ Built |
| | | FAT32 (write, mkdir, LFN, FSInfo) | ✅ Built |
| | | fd layer (base 3, create-on-open, `kcontext` unwind) | ✅ Built |
| | | Page/buffer cache | ⏳ Later |
| 6 | Syscall ABI + libc | Syscall surface (split: fds / handles) | ✅ Built (13 calls: write/read/open/close/lseek/stat/readdir/spawn/wait/yield/getpid/kill/exit, user-pointer-validated; process/thread objects now go through the typed-handle half of the split too — `sys_spawn`/`sys_wait`/`sys_kill` take/return capability handles, not raw pids, see §3.4) |
| | | newlib (bring-up libc) → musl (later) | 🔓 / ⏳ |
| 7 | Userspace runtime | Userspace ELF loader (static-first) | 🎯 Designed *(critical path)* |
| | | init / service manager (PID 1) | 🔓 Open |
| 8 | Core userspace (personality) | Native shell — structured; minimal runner first, ported dash for Linux scripts | 🎯 Designed |
| | | Coreutils (downstream of shell builtins-vs-separate — see §6) | 🔓 Open |
| | | TTY + terminal emulator | 🔓 Open |
| | | Toolchain (TCC → GCC/LLVM, linker, build tool) | 🎯 Roadmap |
| | | Package manager | 🔓 Open |
| | | Editor / IDE (native) | 🔓 Open |
| 9 | Compatibility | Linux-compat mechanism | 🔓 Open |
| | | POSIX-"enough" surface | 🎯 Designed |
| 10 | UI / graphics | Framebuffer console + TTY | 🎯 Designed *(enough for self-hosting)* |
| | | Compositor / display server, toolkit, GUI apps | ⏳ Later *(post-self-hosting)* |
| 11 | Security (cross-cutting) | Ring 0/3 + address-space isolation | 🎯 Designed |
| | | Access control (see §3.5) | 🎯 Designed |
| | | POSIX-capabilities (split root), multi-user policy | ⏳ Later |

---

## 3. Settled Architectural Decisions

### 3.1 Kernel structure — **Hybrid / modular-monolithic**

**Decision.** All kernel-side components (drivers, block layer, VFS, filesystems) run in ring 0, one address space, direct calls. Every subsystem exposes an **ops vector** (function-pointer struct), following the VFS template already in the tree; call sites go through the indirection, never into a subsystem's internals. "Modular" means *source*-modular (clean compile-time interfaces) now; runtime-loadable modules are a later luxury.

**Why.** Fastest route to a usable dev environment this year, best raw performance, and it's what the working storage/filesystem stack already is — a microkernel would rework it for isolation not yet needed. Kernel structure is not where personality lives, so choosing monolithic-ish costs almost no identity. The ops-vector discipline is the exact seam that could later become a message-send, keeping the microkernel door open **per-component** rather than as an all-or-nothing bet.

**Caveats.** Fault model stated plainly: a bug in a driver or filesystem can corrupt kernel memory or panic the whole system. The mitigation (lift a flaky component into a userspace server) exists *because of* the ops seam, but is not taken now.

**Consequence.** Settles "where do drivers live" → in-kernel, by default.

### 3.2 Process creation — **`spawn()` native, `fork()` later as compat**

**Decision.** `spawn()` (shape of `posix_spawn`/`CreateProcess`) is the blessed primitive: build the new address space directly from the ELF, apply a list of *file actions* (redirections, pipe wiring); the caller is not duplicated. `fork()` + `exec()` added later as a compatibility syscall, gated on a program worth porting that needs it.

**Why.** `spawn()` is cleaner, thread-safe, fast in the fork-then-exec common case, and natural coming from embedded — a justified divergence (`fork()` is the subject of "A fork() in the road", HotOS 2019). The asymmetry is decisive: `spawn()` is trivial to build on `fork()`+`exec()`, but `fork()` is essentially impossible to build on `spawn()`, so the primitive choice determines what's even available. `fork()`'s implementation falls out naturally once COW lands.

**Consequence.** COW is **only** truly needed by `fork()`, which is deferred — so **COW moves off the self-hosting critical path** and arrives with the `fork()` compat work. Do not let it get re-added as a near-term requirement.

### 3.3 IPC & interrupts — **Message ports + uncatchable kernel kill; pipes; signals later**

**Decision.**
- **Pipes** (forced, not a fork): required for shell pipelines and `make`. The only sub-decision is the *create-pipe-and-wire* file-action in `spawn()`; exact API settled at ABI time.
- **Interrupt/event model:** each process has a **message/event queue** with an event loop; "interrupt" means "a message arrives, handled when next polled" (cooperative). Ctrl-C, child-exit, etc. are messages.
- **Uncatchable kernel-level kill** (`SIGKILL`-equivalent) exists **from day one** — the kernel can terminate a PID regardless of what it's doing, because a message port alone cannot stop a process that won't read it. Ships with the scheduler.
- **Unix signals** emulated as a later compat layer over the message port.

**Why.** Signals are a canonical historical-accident-not-optimal design: handlers run at arbitrary instruction boundaries (tiny async-signal-safe function set), interact badly with threads, and produce `EINTR`. The message-port model has none of these problems, is thread-safe by construction (matters for SMP), is far easier to implement *correctly* (decisive for a solo author), and matches the ops-vector-as-future-message-send seam. The divergence pays off specifically *because* we don't want all of Linux's software — the shim cost is low, the correctness win is high.

**Caveat.** Cooperative interruption requires the uncatchable kernel kill for runaway processes — non-optional, ships immediately.

### 3.4 Syscall ABI — **`SYSCALL`/`SYSRET`; fds for streams, typed handles for structure**

**Mechanism (fixed by hardware, not a choice).** `SYSCALL` via MSRs (`LSTAR` entry, `STAR` segments, `FMASK` flag mask; SCE bit in `IA32_EFER`). Syscall number in `rax`; args in `rdi, rsi, rdx, r10, r8, r9` (`r10` not `rcx`, because `SYSCALL` clobbers `rcx` for the return address; `r11` holds saved flags); return in `rax`. *(Intel SDM Vol. 3A §5.8.8.)*

**Decision — split the model by the nature of the thing:**
- **File descriptors** for byte-stream things — files, pipes, TTYs — reached by `read`/`write`/`close`. Kept because it's genuinely best for streamy/redirectable I/O **and** it's exactly what newlib's stubs (`_read`, `_write`, `_open`) and the toolchain assume, making the libc port a near-straight-through mapping.
- **Typed handles** for structural objects — process, thread, memory-region, message-port — opaque handles with operations typed to the object (Zircon/Fuchsia shape). Used because these are precisely what Unix jammed awkwardly into the file model (`/proc`, the `ioctl` junk drawer); the object model gives clarity and type-safety here at no compat cost, since libc doesn't care how a process is created.

**Rider.** `errno` convention (negative return / `errno`) rides along for fd-based file calls via newlib. Handle-based object calls are new surface with freedom to return a clean explicit status/result instead of errno-in-a-global.

**Why.** "Everything is a file" is half-right: right for true byte streams, wrong for the things Unix forced into the shape — `/proc` and `ioctl()` are where the abstraction visibly cracks. Splitting on that line is the technically-justified boundary *and* the lower-compat-cost boundary.

### 3.5 Security & access control — **Unix uid/gid/mode on disk; capabilities for the object world**

**Decision.**
- **Object world** (processes, threads, memory, ports, anything reached by a handle): **capabilities** — you may operate on an object only if you hold a handle to it; no ambient authority. This is mostly *free* from the §3.4 handle decision (a handle is a capability in embryo).
- **On-disk, path-reached files:** classic **Unix owner / group / rwx mode**, checked at `open()`. Kept because EMBKFS/FAT32 metadata, newlib's `_fstat`, and `ls -l` already speak it — keeping the toolchain port straight-through instead of inventing file-capabilities on the critical path.

**Why.** Same cut as §3.4, drawn one layer down: in-memory handed-out objects → capabilities; on-disk named things → Unix permissions. Pure file-capabilities is a genuine research frontier and heavy porting friction, squarely on the critical path — the most expensive place to invent, for security not yet needed on a single-user dev box.

**Now vs. later.** uid/gid metadata is present in the on-disk format (get the shape right to avoid a later migration), but multi-user policy (login, `/etc/passwd`, isolation) is deferred. The coarse-root problem's known fix — POSIX capabilities splitting root into independent bits (`CAP_NET_BIND_SERVICE` etc.; *not* the same "capability" as object handles) — is a natural later refinement of the Unix side.

---

## 4. Built Subsystems (current kernel)

- **Boot:** custom two-stage BIOS bootloader, LBA via INT 13h extensions, E820 memory map, ELF kernel loader.
- **CPU/interrupts:** GDT/IDT/TSS, exception handling, ACPI/MADT parsing, APIC (8259 PIC retired).
- **Memory:** bitmap PMM, higher-half VMM (kernel @ `0xFFFFFFFF80100000`). *Known limits: maps only first 1 GB of physical RAM (breaks on real HW with more), hardcoded stack, no NX.*
- **Storage:** AHCI driver (LBA48 read/write, verified vs. host ground truth), block layer with DMA bounce buffers (IDE primary channel for DMA; test disks as `sdb`).
- **Filesystems:** VFS (filesystem-neutral mount registry, `vfs_resolve` with `.`/`..` breadcrumb stack, `vget` op, `ls`, live boot selftest). FAT32 (file write, `mkdir`, LFN with correct reverse on-disk ordering + checksums, FSInfo; clean vs. `fsck.vfat`/`mcopy`). EMBKFS (on-disk format, B-tree descent, mount-time allocator, CoW write path with superblock-swap atomicity, block reclaim; Python oracle produces byte-identical checksums).
- **File layer:** fd table (base 3, stdio reserved), create-on-open, `kcontext` setjmp/longjmp so `sys_exit` unwinds to kernel (RFLAGS saved/restored — bypassing `iretq` had left IF cleared). Unlink-while-open safety via `g_open_refs` + deferred-free. *Known limit: no on-disk orphan list; mount-time sweep planned.*

---

## 5. Roadmap — Critical Path to Self-Hosting

Ordered by dependency. Fine-grained tasks live in `TODO.md`.

1. **Ring 3 entry** ✅ — via `int 0x80`/`iretq`, privilege separation working. The `SYSCALL`/`SYSRET` fast path (MSR setup, entry stub) is still 🎯, but no longer blocks anything below it.
2. **Per-process address spaces** ✅ — separate page-table hierarchies; `process_create` builds one from an ELF path per §2's `spawn()`-shaped row.
3. **Scheduler** *(Phases A–D landed)* — timer-driven preemptive priority-band round-robin with aging, wait queues, the **uncatchable kernel kill**, real blocking `wait()`, and capability handles for `spawn`/`wait`/`kill` are all built. Only SMP is still open, deferred until something concrete needs it. Full phased spec: [`architecture/process-and-scheduling.md`](architecture/process-and-scheduling.md).
4. **Userspace ELF loading** ✅ — static binaries, loaded from the filesystem (not an embedded blob); relocation/dynamic linking still later.
5. **Syscall ABI surface for a libc** *(largely built)* — file I/O ✅ (open/read/write/close/lseek/stat/readdir, wired through to VFS/FAT32/EMBKFS), process spawn/wait ✅ (`sys_spawn`/`sys_wait`, still busy-poll-based — see the scheduling spec §7.4/§17), user-pointer validation ✅ (`access_ok`/`copy_from_user`/`copy_to_user`). Still open: memory (`brk`/`mmap`-equiv), basic TTY, and the `SYSCALL`/`SYSRET` fast path.
6. **newlib** — supply the ~dozen low-level stubs (`_read`, `_write`, `_open`, `_close`, `_lseek`, `_sbrk`, `_fstat`, `_exit`, `_getpid`, …). Familiar from Cortex-M.
7. **TTY/console** — keyboard input + the VBE framebuffer console.
8. **Toolchain port — TCC first.** Single self-contained binary (built-in assembler + linker, no subprocess spawning), so it runs before a full `fork`/`exec`/pipe process model exists. GCC/LLVM later, once the subprocess-spawning model is in place.
9. **Shell + editor** — the last mile to a livable environment.

Steps 1–5 are the natural continuation of the existing kernel. Step 8 (TCC on metal) is the flag for "true self-hosting achieved."

---

## 6. Personality Layer — Decided & Open Forks

**Decided:**

- **Native shell** `🎯` — **structured data model** (Nushell/PowerShell lineage): typed records/tables flow through pipelines, no text-parse fragility. Interop at the boundary — external programs run; their text output is parsed into structure *explicitly* (`from json`/`from csv` style), never by magic. Compat: the ported **dash** (`/bin/sh`) carries all Linux-script duty, freeing the native shell from compat obligations entirely. **Sequencing:** a **minimal command runner** during bring-up (spawn builds, wire pipes + redirections, livable) → the **full structured shell built later, inside EmbLinkOS** (dogfooding the dev environment). *Not on the self-hosting critical path — must not be allowed to delay it; the toolchain bootstrap needs the ported dash, not the native shell.*

**Open, in decision order:**

1. **Shell: in-process builtins vs. separate programs** `🔓` — builtins pass live structured values with no serialization, but coreutils then largely *become* builtins; separate programs keep the Unix shape but require a **serialization format over the kernel's byte pipes** (§3.3) plus a convention every native tool follows. Decides the shell's internals *and* what "coreutils" means — so it comes **before** coreutils.
2. **Coreutils** `🔓` — write native vs. port. Largely collapses into the shell if the answer to (1) is builtins.
3. **Linux-compat mechanism** `🔓` — recompile-from-source compat (aligned with "not all of Linux") vs. binary syscall emulation.
4. **Editor / IDE** `🔓` — native. Highest-leverage identity move available and *no* ecosystem tax (it's ours regardless); does not conflict with self-hosting the way a novel C ABI would.

Smaller open items: pipe file-action API shape (at ABI design), init/PID 1 model, package manager, build tool (`make` vs. own), TTY/terminal-emulator design.

---

## 7. Deferred / Later (off the near-term critical path)

UEFI bootloader · buddy allocator · COW pages + NX (arrive with `fork()`) · runtime-loadable kernel modules (in-kernel ELF relocator + symbol table) · page/buffer cache · NVMe · USB isochronous transfers / xHCI hub support · GUI stack (compositor, toolkit, apps) · SMP · ARM64 portability · on-disk orphan list + mount-time sweep · POSIX-capabilities (split root) · multi-user policy (login, `/etc/passwd`) · musl (replacing newlib).

---

## 8. Conventions (carried from the codebase)

- **kprintf:** `%lu`/`%lx` for 64-bit; `%08X` with cast for 32-bit. Build flags omit `-Wall` (packed-member warnings suppressed by design).
- **Packed-member access:** `memcpy` into aligned locals — standard throughout.
- **DMA:** heap addresses may exceed the 32-bit PRDT limit; bounce-buffer logic lives once in the block layer, not per-driver.
- **Verification:** oracle-first — Python formatter/verifier is ground truth; the C reader is validated against it.
- **Git hygiene:** PR-based workflow, `Teo` branch; after a GitHub-side PR merge, fetch → check log → merge master into `Teo` (not blind force-push). Non-fast-forward rejections from PR merges / CodeQL auto-fix commits are expected.
- **Hardware:** consult the Intel SDM before coding any hardware-level component.
