# EmbLinkOS

A 64-bit x86_64 operating system built entirely from scratch — no GRUB, no
pre-built bootloader, no borrowed kernel. Boot sector, page tables, scheduler,
filesystem, USB stack, GPU driver, dynamic linker, and its own GUI toolkit are
all written and understood by hand, in this repo.

It's no longer just a kernel exercise: EmbLinkOS boots straight into a
graphical desktop (`home.elf`), runs multiple windowed apps with a real
compositor, and has a from-scratch UI toolkit (**EmUI**) that friends can
build apps against without touching kernel code.

**It also compiles C on itself — real C, headers and all.** A C compiler (TCC)
runs as a normal process and builds working programs without ever leaving the
OS: `#include <stdio.h>` resolves against the on-image newlib headers, `-lc`
links the real `libc.a`, and the result runs. The OS has even **rebuilt one of
its own tools from source** (`tally`, compiled and linked on-OS, then run
through the shell's live pipeline against the original as an A/B oracle). It
hosts **C++/libstdc++**, **CPython 3.14** and **git 2.49** too, none of them
forks. All of that on an OS that is deliberately **not POSIX**: there is no
`fork`/`exec` here, which is exactly why the compiler is TCC (one
self-contained binary) and not GCC (a driver that fork/execs `cc1`/`as`/`ld`,
and so is structurally impossible here). See [docs/PORTS.md](docs/PORTS.md).

## What it is

EmbLinkOS starts from a 512-byte boot sector and works its way up: real mode
→ protected mode → long mode → a higher-half kernel at
`0xFFFFFFFF80100000`. From there: physical/virtual memory management,
interrupts and APIC, a preemptive SMP scheduler with ring-3 processes and
threads, a custom journaling-and-more filesystem (EMBKFS) with encryption and
snapshots, USB (xHCI/EHCI/UHCI/OHCI) keyboard and mass storage, a GPU driver
(virtio-gpu/Bochs DISPI/VBE) with a window compositor on top, a newlib-based
userland with an in-kernel dynamic linker, and EmUI — a SwiftUI-flavored UI
toolkit apps are built against.

## Features

**Boot & core**
- Custom two-stage BIOS bootloader (E820 memory map, A20, protected → long
  mode, ELF kernel loader).
- Higher-half kernel, linker-script physical/virtual split.
- Full IDT/ISR exception handling; APIC (Local APIC + IO-APIC), SMP bring-up
  across all detected cores via ACPI/MADT.
- Physical (bitmap) and virtual (4-level paging, direct map) memory managers.
- Serial + in-kernel `kprintf`, plus a VBE/GPU framebuffer console.

**Processes & syscalls**
- A preemptive, SMP-aware scheduler; ring-3 processes *and* ring-3 threads
  with join, spawn()-based process creation (no fork/exec), a capability-
  style handle table for cross-process references (windows, IPC endpoints).
- An `int 0x80` syscall ABI, user-pointer validation (`copy_from/to_user`),
  and an in-kernel ELF loader supporting both static and **dynamically
  linked** userland binaries — see [BUILD_SETUP.md](docs/BUILD_SETUP.md#dynamic-linking-how-libembkso-actually-works).
- IPC: handles, channels, and endpoints for cross-process communication.

**Storage**
- **EMBKFS**, a from-scratch filesystem (B-tree-backed, checksummed,
  AES-256-XTS encryption, compression, snapshots, self-heal, provenance,
  verified boot) — see [docs/EMBKFS_spec_v2.2.md](docs/EMBKFS_spec_v2.2.md).
- FAT32 (read), ATA/AHCI (SATA), MBR partitioning, block-device abstraction
  usable over either ATA or USB mass storage.

**I/O**
- Full USB stack across all four host-controller generations (xHCI, EHCI,
  UHCI, OHCI) — keyboard, mouse, and mass storage.
- PCI enumeration, HPET/PIT/RTC timers.

**Display & UI**
- GPU driver with automatic fallback: virtio-gpu → Bochs DISPI → VBE.
- An in-kernel **window compositor** (`kernel/gfx/compositor.c`): z-ordered
  windows, kernel-drawn or app-owned ("chromeless") chrome, zero-copy
  shared-memory windows, resizable windows, and always-on-desktop widgets in
  their own z-band.
- **EmUI**, a from-scratch, SwiftUI-flavored declarative UI toolkit
  (`ui/`) apps are built against — see [docs/EMUI_GUIDE.md](docs/EMUI_GUIDE.md)
  to build one, or [docs/EMUI_INTERNALS.md](docs/EMUI_INTERNALS.md) for how
  it's built.
- Boots straight into a graphical home launcher with an app grid and a live
  desktop widget, not a text shell.

**Userland**
- A newlib-based libc port (freestanding, static-newlib, and
  dynamically-linked-against-`libembk.so` build modes) — see
  [user/README.md](user/README.md).
- **A C compiler on the OS** — TCC compiles, assembles and links real
  `#include`-using programs against the on-image newlib headers and `libc.a`,
  entirely on the metal (`test tcc real` → `exit=42`, byte-exact stdio output;
  `test tcc tally` → the OS rebuilds its own `tally` tool from source and runs
  it through the shell's pipeline).
- **C++/libstdc++, CPython 3.14, git 2.49** run as ordinary processes —
  see [docs/PORTS.md](docs/PORTS.md).
- A structured, Nushell-flavored **shell** with typed pipelines —
  see [docs/SHELL.md](docs/SHELL.md).
- **Interruption** (Ctrl-C) without signals: a routed, capability-scoped
  cancellation that blocking syscalls observe — see
  [docs/INTERRUPTION.md](docs/INTERRUPTION.md).

## Building and running

You'll need an `x86_64-elf` cross-compiler (with newlib), NASM, QEMU, and GNU
Make. If this is a fresh clone and you don't have the cross-compiler yet,
start with **[docs/BUILD_SETUP.md](docs/BUILD_SETUP.md)** — it walks through
building the toolchain, the newlib rebuild this project needs for full C99
printf support, and first boot.

```bash
# Build the kernel + bootloader image
make

# Build every userland app + the shared UI toolkit, pack them onto a disk image
make embkfs.img

# Boot straight into the graphical desktop
make run-embkfs-tree

# Boot paused for GDB on :1234
make debug
```

In a second terminal, attach the debugger:
```bash
gdb        # .gdbinit auto-connects to QEMU and breaks at kernel_main
```

See [CONTRIBUTING.md](CONTRIBUTING.md#build--run) for the full list of
`run-*` targets (AHCI, USB variants, encrypted EMBKFS volumes, multi-core,
KVM acceleration, host-side UI unit tests, ...).

## Documentation

| Doc | For |
|---|---|
| [docs/BUILD_SETUP.md](docs/BUILD_SETUP.md) | First-time setup: toolchain, newlib, dynamic linking, troubleshooting a fresh clone |
| [docs/EMUI_GUIDE.md](docs/EMUI_GUIDE.md) | Building a UI app with EmUI, start to finish |
| [docs/EMUI_INTERNALS.md](docs/EMUI_INTERNALS.md) | How EmUI itself is built, for anyone extending the toolkit |
| [user/README.md](user/README.md) | The userland SDK layers and newlib port details |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | System-wide design decisions and the current stack |
| [docs/PORTS.md](docs/PORTS.md) | C++, CPython, git and the C compiler on the OS — what works, what honestly can't |
| [docs/SHELL.md](docs/SHELL.md) | The structured shell: typed pipelines, builtins, semantics |
| [docs/INTERRUPTION.md](docs/INTERRUPTION.md) | Ctrl-C without signals: how cancellation is routed and observed |
| [docs/PROJECT_STATUS.md](docs/PROJECT_STATUS.md) | Phase-by-phase build history |
| [docs/TODO.md](docs/TODO.md) | Open items by subsystem |
| [docs/architecture/process-and-scheduling.md](docs/architecture/process-and-scheduling.md) | Process/thread/scheduler deep-dive |
| [docs/EMBKFS_spec_v2.3.md](docs/EMBKFS_spec_v2.3.md) | The filesystem: latest additions (atomic rename, extents, the read path) |
| [docs/EMBKFS_spec_v2.2.md](docs/EMBKFS_spec_v2.2.md) | The filesystem's on-disk format (compression, encryption, snapshots) |
| [CONTRIBUTING.md](CONTRIBUTING.md) | How to make and prove a change in this repo |

## Project layout

```
boot/stage1/, boot/stage2/   Bootloader: MBR sector, then E820/A20/long-mode/ELF loader
kernel/
  arch/x86_64/                IDT/ISR, GDT, APIC, SMP bring-up, syscalls, the in-kernel ELF/dynamic loader
  mm/                          Physical + virtual memory managers
  process/                     Processes, ring-3 threads, the scheduler
  fs/                          VFS, EMBKFS, FAT32
  drivers/                     Storage (ATA/AHCI), USB (xHCI/EHCI/UHCI/OHCI), video, input, timers, PCI
  gfx/                         The window compositor
  ipc/                         Handles, channels, endpoints
  crypto/                      AES-XTS, SHA-256, HMAC, PBKDF2 (for EMBKFS encryption)
ui/                            EmUI: the from-scratch UI toolkit (scene graph, layout, reactive core,
                                declarative API, theming, widget kit, render backend, the SwiftUI-flavored DSL)
user/
  lib/                         The userland SDK (embk.h) + newlib retargeting (crt0.c, syscalls.c)
  bin/                         Userland programs, including the home launcher and EmUI apps
tools/embkfs_mkfs/             Python reference implementation: builds and verifies EMBKFS images
tools/{tcc,cpython,git,zlib,cxx}/  Build scripts + patches for the ports (see docs/PORTS.md)
docs/                          Architecture, setup, and UI documentation
```

## License

Released under the MIT License. See [LICENSE](LICENSE).
