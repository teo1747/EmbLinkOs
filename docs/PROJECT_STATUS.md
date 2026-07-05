# EmbLinkOS — Project Status & Handoff

*Last updated: 2026-07-05. Living document — reconciled from two previously
diverging copies (root `PROJECT_STATUS.md` and this file) into one canonical
version. If you're returning to this after time away, read `ARCHITECTURE.md`
first (the intended design and the decisions behind it), then this file
(what's actually built), then `TODO.md` (what's left). The repo and git
history are ground truth; this file is reconstructed for the design record —
if it disagrees with the code, the code wins.*

## Project Overview

Building a complete x86_64 operating system from absolute zero — custom
bootloader, kernel, filesystem, userspace, all hand-written with the goal of
understanding every line. No GRUB, no shortcuts. Long-term goal: run real
software, with the OS eventually self-hosting and supporting ARM64 + SMP.

EmbLinkOS today is a 64-bit x86 kernel with:
- **Bootloader**: BIOS-based 2-stage loader with hardcoded kernel layout
- **Memory**: PMM + VMM with dynamic paging (up to 4GB sustained, ~32GB architectural limit)
- **Filesystems**: EMBKFS (native, transactional CoW, Merkle-checksummed), FAT32 (R/W), VFS multiplexer
- **Drivers**: Serial console, ATA/DMA, AHCI, keyboard, VBE (EDID + mode enumeration fallback)/Bochs-DISPI/VirtIO-GPU framebuffer, ACPI/APIC, xHCI/EHCI/UHCI/OHCI USB (HID keyboard + mass storage + hub support on the legacy HCs)
- **Interrupts**: IDT + handler dispatch, exceptions, LAPIC timer (now driving preemption), keyboard IRQ
- **User Mode**: Ring-3 entry via iretq, ELF64 loader, int 0x80 syscalls (write, exit, yield, open, close, read, lseek, stat, readdir, spawn, wait, getpid), validated user pointers (`access_ok`/`copy_from_user`/`copy_to_user`)
- **Processes**: per-process address spaces + guarded kernel stacks + fd tables, timer-preemptive round-robin scheduler, wait queues, uncatchable kill, `sys_spawn`/`sys_wait` (see Phase 17/18)

## Design Philosophy

- Slow and deliberate, not fast — understand every register, bit, and SDM section.
- Build bottom-up; defer nothing that the next layer truly depends on.
- Track open issues in `TODO.md`; record completed work here.
- SMP-aware and portability-minded from the start (arch-specific code kept
  identifiable so an ARM64 port later is a contained campaign, not a rewrite).
- See `ARCHITECTURE.md` §1 for the full governing principle ("bless the clean
  native primitive; provide the compatible one as an opt-in layer") and §3 for
  the settled architectural decisions (`spawn()` over `fork()`, message ports
  over signals, capability handles for the object world) that later phases
  below build on.

## Naming & Family

- Project: **EmbLink** (Embedded + Link).
- **EmbLinkOS** — this project: general-purpose x86_64 → ARM64 OS.
- **EmbLink-RTOS** — separate sibling project: real-time OS for Cortex-M
  (own repo, own kernel; shares brand, philosophy, and copied leaf utilities,
  NOT kernel core). Future goal: the two communicate over a message protocol
  (UART now, AMP later), with EmbLinkOS providing tools for the RTOS.
- Code prefix: `embk_` for kernel-internal subsystems.
- Kernel virtual base: `0xFFFFFFFF80000000` (higher half).
- Repository: `github.com/teo1747/EmbLinkOs`.

---

## Completed Phases

### Phase 1 — Bootloader ✅
- Custom Stage 1 (512 B, real mode, MBR signature, INT 13h).
- Custom Stage 2 (switches Real → Protected → Long Mode).
- LBA disk loading via INT 13h ext 0x42 (per-sector loop).
- E820 memory map query, A20 enable, GDT (32 + 64 bit).
- Dual page-table mapping (identity + higher half), ELF kernel loader.

### Phase 2 — IDT & Exceptions ✅
- 256-entry IDT, ISR stubs for exceptions 0–31 (NASM).
- C handler dumps register state; catches divide-by-zero, page fault, etc.
- Page-fault handler decodes CR2 + error-code bits (P/W/U/RSVD/I) per SDM 3A §4.7.

### Phase 3 — Physical Memory Manager ✅
- Bitmap allocator, dynamically sized from E820 highest address.
- Bitmap placed at kernel_end; reserves low memory, kernel, bitmap, stack.
- pmm_alloc_page() / pmm_free_page() return/accept physical addresses.

### Phase 4 — Higher-Half Kernel + VMM ✅
- Linker links the kernel at 0xFFFFFFFF80100000; far jump to higher half.
- Custom 4KB-page VMM: vmm_map / vmm_unmap / vmm_get_phys / vmm_flush_tlb.

### Phase 4.1 — Full Direct Map + MMIO mapping ✅
- Linux-style layout: direct map of all usable RAM via 2 MB huge pages.
- Separate KV2P/KP2V (kernel range) vs V2P/P2V (direct map) macros.
- vmm_map_mmio(): dynamic MMIO mapping at MMIO_BASE (4 KB pages, NOCACHE).

### Phase 5 — kprintf ✅
- Format support: %d %u %x %X %p %s %c %% with width + zero-pad, %l/%ll.
- Built on GCC __builtin va_list.
- Later refactored to share a core with snprintf (see Core Library below).

### Phase 6 — Framebuffer + Console ✅ *(superseded — see Phase 16)*
- VBE mode 0x118 (1024×768×24bpp, LFB) set in Stage 2, info passed to kernel.
- Framebuffer mapped via vmm_map_mmio; fb_put_pixel / fb_clear / fb_draw_char.
- Embedded IBM VGA 8×16 font (public domain), ASCII 0x00–0x7F.
- Console abstraction: cursor, colors, scrolling, dual backend (serial + FB).
- kprintf routes through console → appears on both serial and screen.
- *This hardcoded-VBE-only design was replaced by the GPU-abstraction +
  double-buffered framebuffer rewrite in Phase 16; kept here as the historical
  record of how the original framebuffer was built.*

### Phase 7 — Hardware Interrupts (PIC + Timer + Keyboard) ✅
- Kernel GDT (null + kernel code/data), replacing the bootloader GDT.
- 8259 PIC driver (remapped 0x20–0x2F, EOI ordering, IMR mask/unmask).
- IRQ dispatcher: 16 stubs → IDT 32–47, irq_register installs + unmasks.
- PIT timer (IRQ 0), PS/2 keyboard (IRQ 1, US QWERTY, circular buffer).

### Phase 8 — Kernel Heap + Spinlocks ✅
- Linked-list first-fit allocator (kmalloc/kfree/kcalloc/krealloc), 16-byte
  aligned, block split + bidirectional coalesce, auto-grows from PMM.
- Heap canaries + kheap_check/kheap_stats.
- spinlock_t (atomic test-and-set + IRQ save/restore); heap is IRQ-safe.
- types.h (NULL, bool, size_t).

### Phase 9 — APIC (retires 8259 PIC) ✅
- Local APIC mapped + enabled (MSR + spurious vector).
- LAPIC timer: PIT-calibrated, periodic 100 Hz, vector 48, LAPIC EOI.
- IO-APIC redirection programming; keyboard GSI 1 → vector 33 → CPU 0.
- 8259 fully masked/retired. Interrupt path: device → IO-APIC → LAPIC → CPU.
- ACPI: RSDP/RSDT/XSDT/MADT parsed (CPUs + interrupt controllers enumerated).

### Phase 10 — PCI ✅
- Legacy config access (0xCF8/0xCFC), full bus/device/function scan.
- Per-device vendor/device/class/subclass/prog_if/header type table.
- BAR parsing: read + size all 6 BARs (I/O, 32/64-bit MMIO), prefetch detect.
- io.h gained inw/outw/inl/outl.

### Phase 11 — Storage (ATA + AHCI) ✅
- **ATA PIO** (polled): multi-drive detection (primary/secondary × master/slave),
  IDENTIFY, LBA28 (CHS fallback), read/write, per-drive struct + model string.
- **ATA interrupt-driven**: IRQ 14 → vector 46; read IRQ-before-transfer, write
  poll-DRQ-then-IRQ-after, cache-flush IRQ-confirmed. IRQ count == op count.
- **ATA DMA** (bus mastering): READ DMA (0xC8) + WRITE DMA (0xCA), single-entry
  PRDT in BSS (KV2P), ≤64KB, completion via IRQ 14, zero-copy, byte-exact.
- **AHCI/SATA**: controller discovery, ABAR map, AHCI mode, port enumeration;
  command list / FIS / command-table machinery; IDENTIFY; READ DMA EXT (0x25)
  + WRITE DMA EXT (0x35), LBA48; per-port BSS memory. Verified read+write
  against host-visible ground truth on a 64 MB SATA disk.
- Original stated project goal (DMA + AHCI) reached.

### Phase 12 — Block Device Abstraction ✅
- `struct embk_block_device`: uniform interface over storage drivers via
  read/write function pointers + driver_data (the C polymorphism pattern).
- Registry with auto-naming (sda, sdb, sdc…), lookup by index or name.
- Bounds-checked embk_block_read/embk_block_write returning EMBK_* error codes.
- ATA adapter (both IDE drives, chunked to 64-sector DMA limit) and AHCI adapter
  (per-port sector count from IDENTIFY at init).
- Verified: 3 disks (2 IDE + 1 AHCI) enumerate through one interface; reads and
  writes dispatch to the correct driver with no driver-specific call-site code.
- USB mass storage joined this same registry in Phase 16 (`usb_core.c`'s
  BOT/SCSI driver registers a block device exactly like ATA/AHCI do).

### Phase 13 — FAT32 (read + write) ✅
- On-disk structures: boot sector / BPB parse, FAT mirroring, FSInfo.
- Cluster-chain traversal; directory iteration handling both 8.3 short names and
  LFN (long-file-name) entries, with LFN checksum validation.
- Read path: resolve by path, read file data across cluster chains.
- Write path: file create + write, directory-entry allocation, `mkdir`, cluster
  allocation + FAT update, FSInfo free-count maintenance.
- Oracle-validated: images verified clean by `fsck.vfat`, and files round-tripped
  against host `mcopy` (mtools) byte-for-byte.
- Mounts on the block layer; FAT32 test disk is on IDE primary slave (= sdb),
  because block-layer DMA is only wired for the IDE primary channel (see TODO).

### Phase 14 — EMBKFS read-only mount ✅
A custom copy-on-write, Merkle-checksummed filesystem with its own on-disk
format (see `EMBKFS_Specification`, plus `EMBKFS_spec_corrections.md` for the
v2.0→v2.1 fixes found during this work). The read side was built and validated
**oracle-first**: a Python formatter (`mkfs_embkfs.py`) writes known-good images
and a verifier (`verify_embkfs.py`) is the ground truth; the kernel C reader is
checked against them, never the reverse.
- **Format structs**: byte-exact, `_Static_assert`-sized (block ptr 32, key 24,
  node header 40, internal slot 56, item header 32, inode 128, extent 64,
  superblock 160).
- **CRC32C** (Castagnoli): dependency-free software port of the Python oracle;
  gate vector `crc32c("123456789") == 0xE3069283`.
- **Superblock**: read at a fixed *byte* offset (65536) to break the bootstrap
  cycle (block size itself lives in the superblock); magic + version + body
  checksum verified.
- **Node integrity, every read**: a node's own checksum over its block, AND the
  Merkle link against the parent pointer's stored checksum, plus generation and
  self-block-number. The superblock is the root of trust; the check is generic
  over leaf and internal nodes alike.
- **Field-wise key order**: keys compare as the integer tuple (object_id, type,
  offset) — not raw little-endian bytes. Strictly-increasing-key invariant
  enforced within a leaf.
- **Path resolution**: root directory inode → directory entry by CRC32C
  name-hash → authoritative byte-for-byte name compare → file inode + extent →
  read and checksum the data over its logical size.
- **Hash-collision handling**: a real same-length CRC32C collision was found
  ("wgyehkb.txt" / "illoeuw.txt", both 0xC38842AB). Colliding names share one
  directory-entry item as a chain of records (bounded by the item size, no count
  field), walked and distinguished by name; both resolve to distinct objects.
  Validated end-to-end (formatter, verifier, kernel) and kept as a regression.
- **B-tree descent**: the reader handles internal nodes (level > 0), not only a
  single root leaf. Descent picks the rightmost slot whose key is ≤ the target
  (upper-bound search, so an exact boundary key descends right), follows the
  child pointer, and recurses — node verification extending the Merkle chain to
  full tree depth. Validated on a 2-level image whose split lands a key exactly
  on a slot boundary (the ≤-not-< trap).
- Both a flat image and a 2-level tree image boot green; all files resolve with
  end-to-end data integrity verified.

#### Supporting infrastructure ✅
- **errno**: EMBK_* error codes (POSIX-aligned values) + embk_strerror().
  Convention: int + error codes for fallible ops, bool for true/false questions.
- **kstring**: kernel mini-libc (memcpy, memset, memmove, memcmp, strlen,
  strcmp, strncmp, strcpy, strncpy, strcat, strchr) under standard names
  (GCC may emit implicit memcpy/memset).
- **snprintf + kprintf refactor**: both now wrap one format_string core driven
  by an output-sink callback (serial sink vs bounded-buffer sink). snprintf is
  bounds-safe and returns the would-be length (truncation detectable).

#### EMBKFS write path ✅
- **Transactional CoW**: every mutation rebuilds the affected B-tree spine
  bottom-up into fresh blocks, rewrites checksums up the Merkle chain, and
  installs an atomically swapped, generation-bumped superblock — superblock
  write is always last, in-memory state updated only on success.
- **Snapshot-aware allocator**: a per-transaction free delta; superseded nodes
  and freed data runs are reclaimed into the in-memory bitmap on commit.
- **Namespace ops**: create, mkdir (with leaf split when an item overflows a
  node), unlink, rmdir (ENOTEMPTY-guarded), rename (loop-safe across dirs),
  hard link, symlink + readlink. Directory entries are hash-keyed with
  collision chains; `.`/`..` are NOT stored on disk (resolved above the entry
  layer).
- **Object I/O**: write_object_at (sparse, allocates extents on demand),
  read_object_at, append, truncate, resize, seek (SET/CUR/END).
- **Validated** by object-I/O and namespace stress selftests (create/write/
  seek/truncate/resize/append; link/unlink/rename/symlink roundtrips;
  collision-chain shrink; leaf split/merge), all green in QEMU.

#### EMBKFS unlink-while-open safety ✅
- **Unix delete-on-last-close**: an in-memory open-reference table tracks how
  many handles hold each object alive. Free condition is `links == 0 AND
  opens == 0`; `unlink` owns on-disk `links`, the fd layer drives `opens` via
  `embkfs_object_get`/`_put`.
- **Deferred free**: `unlink` on a held-open file drops the name and sets
  `links = 0` but keeps the inode + extents; the orphan is reclaimed by the
  last `object_put`, which consults on-disk `links` as the single source of
  truth (no duplicated "unlinked" flag).
- **Crash-recovery sweep at mount**: on read-write mount, EMBKFS scans the
  live tree for inodes with `links == 0` and reclaims orphan files/symlinks,
  closing the leak window from crashes that happen between unlink and last
  close.
- **Proven**: a file stays readable through its open fd after its name is
  unlinked; blocks are reclaimed only on final close (selftest asserts the
  read-back matches the payload, so an early-free cannot pass).

#### VFS layer ✅
- **Filesystem-neutral op table** (`struct vfs_ops`): lookup, readdir, read,
  write, create, mkdir, unlink, stat, vget, obj_get/obj_put. A NULL op means
  unsupported → the VFS returns -EMBK_ENOSYS. The rest of the kernel never
  calls embkfs_* / fat32_* directly.
- **vnode = stateless value type** `{mnt, ino, type}` — owns nothing, copied
  freely; the VFS core allocates vnode storage, the fs ops only populate it.
- **Mount registry**: static table; `vfs_mount` records an already-open volume
  and wires its root vnode self-referentially (`root.mnt = &slot`). Borrows
  fs_data, never owns it. v1 = single mount at "/".
- **`vfs_resolve` is the sole path parser**: component-by-component walk via
  ->lookup; `.`/`..` handled in the path layer with a value-vnode breadcrumb
  stack (`..` bounded at root, no underflow), depth-bounded. So every fs gets
  dot-dot for free, including ones whose own walker can't.
- **Public surface**: vfs_read/write/readdir/stat (resolve-then-dispatch),
  `vfs_ls` consumer (lists any fs; sizes via vget+stat, dir size tagged as
  entry-count not bytes), boot selftest (6 checks: empty walk, `.`/`..`,
  relative rejection, ENOENT propagation, live readdir-discovered resolve).

#### File-descriptor layer ✅
- **First stateful layer above the fs**: static fd table (fds start at FD_BASE
  = 3, leaving 0/1/2 for stdio); each entry = vnode copy + cursor + flags +
  used. An fd is bound to the OBJECT, not the name → survives rename/unlink.
- **open()**: resolves once; create-on-open under O_CREAT via VFS-level
  parent/leaf split (creates the leaf only, no mkdir -p); O_EXCL → EEXIST;
  obj_get on the convergent path so create and existing-file both register the
  open. O_APPEND honored; O_TRUNC reserved (needs a truncate op).
- **close()**: obj_put BEFORE freeing the slot — this is what triggers reclaim
  of an unlinked-but-open file.
- **read/write** are cursor-driven (advance pos by bytes moved); seek SET/CUR/
  END; fstat; access-mode checks return EBADF. Selftest covers the full round
  trip plus the unlink-while-open invariant end to end through the public fd
  surface.

### Phase 15 — User Mode + Syscalls ✅
- **ELF64 Loader**: parses PT_LOAD, maps segments with correct p_flags (NX,
  W/X); loads `/init.elf` from the filesystem (multi-block extent read), not
  an embedded blob.
- **Ring-3 Entry**: iretq to user code/data, user RSP, EFLAGS=0x202,
  interrupt-live throughout user execution.
- **Context Save/Restore**: `kernel_ctx_save`/`kernel_ctx_restore` capture
  callee-saved regs + RSP + RIP + RFLAGS (software context switch — see
  Phase 17 and `docs/architecture/process-and-scheduling.md` §4.4 for why
  hardware task-switching is deliberately not used).
- **int 0x80 Dispatch**: syscall table with SYS_write (fd=1, serial out) +
  SYS_exit.
- **Per-process address spaces**: `vmm_create_address_space`/
  `vmm_destroy_address_space` (kernel half aliased at PML4 slots 256–511,
  user half private, freed on load failure or process exit);
  `vmm_map_in`/`vmm_get_phys_in`; late CR3 switch; higher-half `p_vaddr`
  rejected at load time.

### Phase 16 — Display Stack + Full USB Host Controller Support ✅
- **GPU abstraction** (`gpu.c`): probes PCI before `fb_init`, prefers
  VirtIO-GPU over Bochs DISPI over the plain VBE fallback (Phase 6's original
  hardcoded path).
  - **VirtIO-GPU** (`virtio_gpu.c`): full virtio 1.x modern PCI transport
    (capability-list walk, split virtqueue), 2D resource scan-out straight
    from the kernel's RAM backbuffer, presented via TRANSFER_TO_HOST_2D +
    RESOURCE_FLUSH per dirty rect — the accelerated host-GPU blit path.
  - **Bochs/QEMU stdvga DISPI** (`bochs_vbe.c`): runtime modeset (no real-mode
    VBE call needed) via I/O ports 0x1CE/0x1CF, default 1024×768×32.
- **Framebuffer rewrite** (`framebuffer.c`): RAM backbuffer with dirty-rect
  `fb_present()`, clipped fill/draw rect, h/v/Bresenham lines, circles
  (outline + filled), screen-to-screen copy, fast memmove scroll, ARGB blit
  with alpha blending; console/bootanim now draw-then-present.
- **USB core** (`usb_core.c`): HCD-agnostic enumeration, data-toggle
  tracking, a HID boot-keyboard driver (with shift map), and a mass-storage
  BOT/SCSI driver that registers block devices (Phase 12's registry) — shared
  by every legacy HC.
- **UHCI** (`uhci.c`, USB 1.x, I/O BAR4), **OHCI** (`ohci.c`, USB 1.x, MMIO
  ED/TD + HCCA periodic table), **EHCI** (`ehci.c`, USB 2.0, async QH/qTD
  schedule, releases full/low-speed ports to a companion controller) — xHCI
  keeps its existing IRQ-driven path; legacy HCs are polled via `usb_poll()`
  from the main loop.
- **Fixed along the way**: `hpet_delay_us` used 1e12 instead of 1e9 fs/µs,
  making every HPET delay 1000× too long — this had silently broken LAPIC
  timer calibration (the "100 Hz" timer actually ticked every ~8.6 s).
- Verified in QEMU per-HC: HID keyboard input + mass-storage block device on
  UHCI, OHCI, EHCI (high speed), xHCI, plus an EHCI+UHCI companion handoff;
  Bochs DISPI + VirtIO-GPU modeset via screendump; VBE fallback under
  `-vga vmware`. No automated selftest yet (see `TODO.md`).

### Phase 17 — Process Lifecycle + Scheduler Bring-Up ✅
Full spec, comparative analysis against Linux/Windows/BSD/XNU, and every bug
below in much greater detail: `docs/architecture/process-and-scheduling.md`.
- **Mechanism**: a static `MAX_PROCESSES = 16` PCB table (`kernel/process/`),
  strict round-robin `schedule()`, `process_create()` builds a fresh address
  space + guarded kernel stack from an ELF path and fabricates a first
  context landing in `process_trampoline`, which `iretq`s to ring 3.
- **Kernel-stack guard pages**: `vmm_alloc_kernel_stack`/
  `vmm_free_kernel_stack` replace a flat `kmalloc` with page-mapped stacks
  with an unmapped guard page directly below — a kernel-mode overflow now
  faults at the overflow site instead of silently corrupting adjacent memory.
- **Zombie reclamation**: `process_reap()`, deferred one `schedule()` call
  behind the actual exit (can't free a stack still executing on) — closes
  what would otherwise be a hard ceiling of exactly `MAX_PROCESSES` process
  creations ever.
- **Three bugs found and fixed getting here** (full postmortems in the spec's
  §16 "Common Bugs"): (1) the ring-3 trampoline pushed a literal `$2` instead
  of the `%2` operand for the CS selector — null-descriptor `#GP` on every
  process start; (2) `kstack_top` was `uint16_t` in a struct storing a 64-bit
  address — silent truncation that would have corrupted `TSS.RSP0`; (3) the
  new kernel-stack VA region was placed in a PML4 slot never touched before
  boot, invisible in the first process's own page tables because
  `vmm_create_address_space()` shares the kernel half by copying PML4 entries
  *by value* at creation time, not by live reference — `#PF` cascading to
  `#DF`. Fixed by reusing `MMIO_BASE`'s already-populated PML4 slot.
- Verified end-to-end in QEMU: `/init.elf` loads from EMBKFS, runs in ring 3
  on a guarded kernel stack, prints via `sys_write`, exits via `sys_exit` —
  zero exceptions.
- **Not yet built at the time**: preemption, blocking/wait queues, priority,
  the uncatchable kernel kill (`docs/ARCHITECTURE.md` §3.3 requires it day
  one), per-process fd tables, `sys_wait`/`sys_spawn`, SMP — all of these
  except priority and SMP were closed in Phase 18 below.

### Phase 18 — Scheduler Phase B, File I/O Syscalls, USB Hubs, VBE/EDID ✅
Closes nearly every gap Phase 17 left open. Full detail and postmortems:
`docs/architecture/process-and-scheduling.md` (§13 roadmap, §16 Common Bugs
6–9) and `TODO.md`.
- **Preemption**: `lapic_timer_handler` now calls `schedule()` every tick
  (100 Hz) — genuine timer-driven round-robin, not just syscall-triggered.
- **Wait queues**: `struct wait_queue` + intrusive `process::wait_next`;
  `wait_queue_block`/`wait_queue_wake_one`/`wait_queue_wake_all`.
- **Uncatchable kill**: `process_kill(pid)` forces `PROCESS_ZOMBIE` from any
  state (unlinking from a wait queue if blocked), redeeming
  `docs/ARCHITECTURE.md` §3.3's "ships with the scheduler" requirement.
- **User-pointer validation**: `cpu/usercopy.c/h` — `access_ok` (range +
  mapped-page check against the caller's own `pml4_phys`), `copy_from_user`,
  `copy_to_user`, `copy_string_from_user`. Wired into every syscall taking a
  user buffer or path.
- **File I/O + process-management syscalls**: `sys_open`/`close`/`read`/
  `lseek`/`stat`/`readdir` wired straight through to the existing VFS;
  `sys_spawn`/`sys_wait` (busy-poll based, see the spec §7.4 for why and
  what's still deferred)/`sys_yield`/`sys_getpid`. 12 syscalls total, up
  from 2.
- **Per-process fd tables**: `struct process` embeds `fds[FD_MAX_OPEN]`
  directly (fs/fd.c's `fd_table()` picks it, or a boot-time-only global
  table before any process exists) — unblocks `spawn()`'s file-action model.
- **USB hub support** (legacy HCs): `usb_core.c`'s `usb_hub_attach` walks a
  hub's ports via class-specific requests and enumerates whatever's
  connected, one level deep, depth-guarded. xHCI's hub gap is unchanged
  (separate code path) and stays documented, not silently open.
- **VBE fallback improved**: `stage2.asm` now queries EDID for the display's
  native resolution and falls back through a real BIOS mode-list
  enumeration (walking `VideoModePtr`) rather than a single hardcoded mode,
  used only when no GPU driver claims the display.
- **Automated selftests**: `test sched roundrobin`/`kill`/`reap`/
  `stackguard` (closes the gap the spec's §12 flagged), `test usb`
  (cross-checks discovered controllers against a PCI rescan), `test gpu`
  (fill_rect/get_pixel/copy_rect round-trip).
- **Four bugs found and fixed getting here** (full postmortems: the spec's
  §16, Bugs 6–9): (6) `kernel_ctx_switch`/`kernel_ctx_save` captured
  RFLAGS from inside an interrupt-gate ISR, where IF always reads 0 —
  every resumed process came back with interrupts permanently masked after
  its first preemption; fixed by forcing IF=1 in the saved snapshot
  (justified: reaching that save point at all proves IF was 1 moments
  earlier). (7) `int 0x80` is also an interrupt gate, so any syscall
  blocking on a hardware completion IRQ (disk I/O inside `sys_open`) hung
  forever; fixed with `sti` as the first instruction of
  `syscall_dispatch()`. (8) `proc_alloc()` marked a brand-new PCB
  schedulable (`PROCESS_READY`) before it was actually built, which the
  scheduler could observe and crash on the instant real preemption and a
  second concurrent `process_create()` call coexisted; fixed by having
  `proc_alloc()` mark `PROCESS_BLOCKED` instead, with `process_create()`'s
  final step the sole place that sets `READY`. (9) none of
  `process_create()`'s four early-return error paths reset `state` back to
  `PROCESS_UNUSED`, permanently leaking the PCB slot on any failed
  creation; fixed by adding the reset to all four.
- **Known, deliberate gap left open**: one further scheduler reentrancy
  hazard was found but *not* fixed this pass — `schedule()` calls made from
  syscall context (`sys_exit`/`sys_yield`) run with IF=1 (needed for Bug 7's
  disk-I/O fix), so a timer IRQ can in principle land mid-`schedule()` and
  re-enter it from the timer ISR. Not observed to misbehave in practice, but
  not actually prevented either — see `docs/architecture/process-and-scheduling.md`
  §8(a) for the honest writeup and the standard fix (bracket `schedule()`
  with `cli`/restore-IF-on-exit), not yet built.

---

## Major To-Do Buckets (Rough Priority)

Full detail lives in `TODO.md`, organized by subsystem. Rough priority order:

### High Priority (block real programs)
1. ~~User-pointer validation~~ — ✅ done, Phase 18.
2. ~~Process & Scheduling gaps (preemption, blocking, uncatchable kill,
   per-process fd tables)~~ — ✅ done, Phase 18 (priority scheduling and SMP
   remain, see Lower Priority below).
3. ~~File I/O syscalls~~ — ✅ done, Phase 18.
4. **A real blocking `sys_wait`**: today's `sys_wait` busy-polls (`process_find`
   + `sys_yield` in a loop) rather than blocking on a wait queue and being
   woken by the child's exit — needs `process::parent`/`zombie_next`
   tracking (`docs/architecture/process-and-scheduling.md` §6.2/§7.4/§17).
5. **The syscall-context `schedule()` reentrancy gap** (Phase 18's "known,
   deliberate gap left open" above) — not observed to misbehave, but not
   actually prevented either; worth closing before it's relied upon.

### Medium Priority (real-world use)
1. **Filesystems**: EMBKFS crash-safe orphan reclaim strengthening, VFS
   multi-mount, `.truncate`/`.readlink` ops; FAT32 IDE secondary channel.
2. **Drivers**: keyboard modifiers/extended scancodes, `vmm_map_mmio_wc`
   (write-combining), USB isochronous transfers, xHCI hub support (legacy
   HCs got hub support in Phase 18; xHCI's is a separate code path, still
   open), USB hot-plug.
3. **Synchronization**: per-CPU heap caches, locks around currently-unlocked
   global tables (per-process fd tables, EMBKFS open-ref table, process
   table) — all deferred until something can actually race (SMP, or the
   Phase 18 reentrancy gap above becoming load-bearing).

### Lower Priority (architecture)
1. **SMP**: AP bring-up (INIT-SIPI-SIPI), per-CPU data, per-CPU LAPIC.
2. **Syscall fast-path**: STAR/LSTAR/SFMASK for `syscall`/`sysret`.
3. **Bootloader v2**: ELF-aware loading, UEFI support, USB/CD boot.
4. **Slab allocator**: fixed-size pools on top of the heap (currently
   linked-list first-fit only).

---

## Known Limits & Caveats

Full list, kept current: `TODO.md`. Summary of the ones most likely to bite:

### Memory
- Bitmap-based PMM is O(n); page tables constrained such that RAM has a
  ~32 GB architectural ceiling today (see `TODO.md`'s VMM section for the
  exact mechanism and the two-phase-paging-bootstrap fix).
- Kernel stack region and heap live in fixed VA ranges chosen to share
  already-populated PML4 slots at process-creation time — see Phase 17's
  Bug 3 above and the spec's §7.2 before adding another such region.

### Filesystems
- No crash-safe orphan reclaim beyond the mount-time sweep (EMBKFS); no
  symlink resolution in the VFS yet (EMBKFS itself supports it).
- Single mount only — no multi-mount / longest-prefix match.

### Drivers
- VBE now does EDID + BIOS mode-list enumeration instead of one hardcoded
  mode — only reached when no GPU driver claims the display (Phase 16
  covers the common case) — but the fallback-of-fallbacks (no VBE at all,
  text mode at 0xB8000) is unchanged and still hangs at `.vbe_failed`.
- USB: hub support on UHCI/OHCI/EHCI (Phase 18); xHCI hub support and
  isochronous transfers on any controller are still open.
- Keyboard: ASCII-only, no modifiers/extended scancodes.

### User Mode / Process
- User pointers are now validated (`access_ok`/`copy_from_user`/
  `copy_to_user`, Phase 18).
- 12 syscalls exist (write, exit, yield, open, close, read, lseek, stat,
  readdir, spawn, wait, getpid) — no `sys_kill` yet (the uncatchable
  `process_kill` is built and used by selftests, just not exposed to ring 3).
- Preemption, wait queues, and the uncatchable kill are built (Phase 18).
  Still open: priority scheduling, a real blocking `sys_wait` (today's
  busy-polls), the syscall-context `schedule()` reentrancy gap noted above,
  and SMP.

---

## Test Status

| Subsystem | Status | Notes |
|-----------|--------|-------|
| PMM | ✅ Passes | Allocates/frees pages consistently |
| VMM | ✅ Passes | Maps kernel + user, NX works |
| EMBKFS | ✅ Passes | Create/read/write/unlink/mkdir/rename/link/symlink verified via selftest; collision-chain and B-tree-descent regressions covered |
| FAT32 | ✅ Passes | Read/write long filenames on MBR partition, oracle-validated against `fsck.vfat`/`mcopy` |
| VFS | ✅ Passes | Path resolution, multi-FS discovery, `ls`, boot selftest (6 checks) |
| File descriptors | ✅ Passes | open/close/read/write/seek/fstat, create-on-open, unlink-while-open, selftest |
| ATA | ✅ Passes | DMA+IRQ on primary channel, MBR partition discovery |
| AHCI | ✅ Passes | Read/write on port 0 (polling), verified against host ground truth |
| Ring-3 Entry | ✅ Passes | ELF loads, user code runs, iretq works, syscalls dispatch |
| LAPIC Timer | ✅ Passes | 100 Hz ticks, TSC calibrated |
| rwlock | ✅ Passes | Reader/writer locking, smoke test passes |
| GPU / Framebuffer | ✅ Passes | `test gpu` (`fb_run_selftests`): fill_rect/get_pixel/copy_rect round-trip, exact colors, boundary non-bleed. Plus manual: Bochs DISPI + VirtIO-GPU modeset verified via QEMU screendump; VBE EDID/enumeration fallback verified (selects a genuinely different mode than the old hardcoded one under `-vga cirrus`). |
| USB (UHCI/OHCI/EHCI/xHCI) | ✅ Passes | `test usb` (`usb_run_selftests`): cross-checks discovered controllers against a fresh PCI rescan. Plus manual: HID keyboard input + mass-storage block device verified per-HC in QEMU, an EHCI+UHCI companion handoff, and a keyboard behind a `usb-hub` on UHCI (hub support, Phase 18). |
| Process lifecycle / scheduler | ✅ Passes | `test sched roundrobin`/`kill`/`reap`/`stackguard` (`process_test_*`, Phase 18) — the four selftests `docs/architecture/process-and-scheduling.md` §12 specifies, all green. Plus manual: `/init.elf` loads, runs in ring 3 on a guarded kernel stack, exits cleanly, zero exceptions; real preemption verified interleaving two kthreads; `sys_spawn`/`sys_wait` verified end to end with real exit codes. |
| Syscalls (file I/O, spawn/wait, user-pointer validation) | ✅ Passes (manual) | Verified via temporary `user/init.c` scaffolds (reverted after each check): file round-trip through `sys_open`/`read`/`write`/`close`; a bad (kernel-address) pointer rejected by `access_ok` instead of dereferenced; spawn/wait observing a real child exit code; per-process fd isolation between two spawned processes. No dedicated automated selftest yet (only the scheduler-side effects are covered by `test sched *` above). |

---

## Architecture

```
   ┌──────────────────────────────────────┐
   │      User Programs (Ring 3)          │
   │  ELF64 @ low-half VA                 │
   └──────────────┬───────────────────────┘
                  │ int 0x80
                  ↓
   ┌──────────────────────────────────────┐
   │      Syscall Dispatcher (Ring 0)     │
   │  write/read/open/close/lseek/stat/   │
   │  readdir/spawn/wait/yield/getpid/    │
   │  exit — user pointers validated via  │
   │  access_ok/copy_{from,to}_user       │
   └──────────────┬───────────────────────┘
                  │
       ┌──────────┼──────────┬─────────────┐
       ↓          ↓          ↓             ↓
    ┌────┐    ┌──────┐  ┌────────┐   ┌───────────┐
    │VFS │    │  FD  │  │ Kernel │   │  Process  │
    │    ├────┤Layer ├──┤Context │   │ & Sched.  │
    └────┘    └──────┘  └────────┘   └───────────┘
       │
    ┌──┴──────────────────────┐
    ↓                         ↓
  EMBKFS                    FAT32
    │                         │
    ├─────────┬───────────────┤
    ↓         ↓               ↓
   Block Subsystem (partition-aware)
    │
    ├─→ ATA (IDE primary + DMA)
    ├─→ AHCI (SATA)
    ├─→ USB Mass Storage (UHCI/OHCI/EHCI/xHCI)
    └─→ Partitions (MBR)

   Display                   USB
    │                         │
    ├─→ GPU abstraction       ├─→ xHCI (IRQ-driven)
    │    ├─→ VirtIO-GPU       ├─→ EHCI (async schedule)
    │    └─→ Bochs DISPI      ├─→ UHCI (I/O BAR4)
    └─→ Framebuffer (VBE      └─→ OHCI (MMIO ED/TD)
        fallback, backbuffer)

   Interrupts / Timers
    │
    ├─→ LAPIC (timer, local)
    ├─→ IO-APIC (keyboard GSI1)
    └─→ IDT (256 vectors, exceptions + IRQs)

   Memory
    │
    ├─→ PMM (page allocator, bitmap)
    ├─→ VMM (paging, NX, per-process address spaces)
    └─→ Heap (kmalloc/kfree, spinlock)
```

---

## Next Steps

Dependency order — see `TODO.md`'s per-subsystem lists for the full detail
behind each item. Everything through Phase 18 (user-pointer validation,
scheduler Phase B, file I/O syscalls, `sys_wait`/`sys_spawn`, per-process fd
tables) is now done; what's left before self-hosting:

1. **A real blocking `sys_wait`** — replace the busy-poll with
   `process::parent`/`zombie_next` tracking (`docs/architecture/process-and-scheduling.md`
   §7.4/§17), and close the syscall-context `schedule()` reentrancy gap
   (§8(a)) while touching this code.
2. **libc port** (newlib first) — implement syscall backends, don't write
   one from scratch — the hinge to running existing software. The syscall
   surface (12 calls, file I/O + process management) is now broad enough to
   make this the natural next step.
3. **Shell**, then coreutils, then a self-hosting toolchain (tcc first),
   then mouse + compositor + GUI. See `docs/ARCHITECTURE.md` §5 for the
   full critical-path roadmap to self-hosting.
4. **Priority scheduling, SMP** — deferred until something concrete needs
   them (see `docs/architecture/process-and-scheduling.md` §17 for why).

---

## Build Environment

- OS: Ubuntu Linux. Toolchain: x86_64-elf cross compiler at `/usr/local/cross/bin`.
- NASM, QEMU, GNU Make, VS Code, GDB (`docs/GDB_CHEATSHEET.md`).
- Repository: `github.com/teo1747/EmbLinkOs`.

## Build / Run Commands

```bash
make                   # build everything
make run               # boot in QEMU (serial → stdio)
make run-ahci          # boot with an extra AHCI SATA disk attached
make run-embkfs        # boot with a flat EMBKFS image as sdb
make run-embkfs-tree   # boot with a 2-level EMBKFS tree image as sdb (has /init.elf)
make run-vga-std       # Bochs DISPI (QEMU -vga std)
make run-virtio-gpu    # VirtIO-GPU accelerated scan-out (QEMU -vga virtio)
make run-usb-uhci      # UHCI + usb-kbd + usb-storage
make run-usb-ohci      # OHCI + usb-kbd + usb-storage
make run-usb-ehci      # EHCI + usb-storage (high speed)
make run-usb-xhci      # xHCI + usb-kbd
make debug             # GDB server on :1234 (paused)
make clean             # remove binaries (preserves disk.img / ahci.img)
```

## Memory Layout

Physical (low region):
```
0x000000 – 0x000FFF   IVT, BIOS data
0x007000 – 0x007FFF   E820 memory map
0x007C00              Stage 1
0x007E00              Stage 2
0x009000 – 0x00CFFF   Bootloader page tables (PML4/PDPT/PD)
0x010000              Kernel ELF (raw, before parsing)
0x100000              Kernel (physical load address)
kernel_end            PMM bitmap, then free pages
```

Virtual:
```
0xFFFF800000000000    DIRECT_MAP_BASE (all usable RAM, 2 MB huge pages)
0xFFFFC00000000000    MMIO_BASE (vmm_map_mmio bump allocator: HPET, LAPIC,
                       IO-APIC, framebuffer LFB, virtio capability windows)
0xFFFFC04000000000    Kernel-stack region (256 GiB into MMIO_BASE's own PML4
                       slot, deliberately — see Phase 17's Bug 3 above)
0xFFFFFF8000000000    KHEAP_BASE (kmalloc/kfree heap)
0xFFFFFFFF80000000    KERNEL_VIRTUAL_BASE (kernel image, higher half)
```

---

*Known limitations (full, current list): `TODO.md`.*
