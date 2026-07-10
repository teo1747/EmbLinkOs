# EmbLinkOS — Project Status & Handoff

*Last updated: 2026-07-10. Living document — reconciled from two previously
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
- **Filesystems**: EMBKFS v2 (native, transactional CoW, Merkle-checksummed, multi-volume, AES-256-XTS encryption, LZ-compression, instant snapshots, self-healing superblock, process-provenance, HMAC verified-root boot check), FAT32 (R/W), VFS multiplexer (real multi-mount, longest-prefix match)
- **Drivers**: Serial console, ATA/DMA (both IDE channels, IRQ14+IRQ15), AHCI, keyboard, VBE (EDID + mode enumeration fallback)/Bochs-DISPI/VirtIO-GPU framebuffer, ACPI/APIC, xHCI/EHCI/UHCI/OHCI USB (HID keyboard + mass storage + hub support on the legacy HCs), CMOS RTC (real wall-clock timestamps)
- **Crypto**: from-scratch SHA-256, HMAC-SHA256, PBKDF2, AES-256, AES-256-XTS (`kernel/crypto/`), every primitive checked against externally-computed known-answer vectors
- **Interrupts**: IDT + handler dispatch, exceptions, LAPIC timer (now driving preemption, per-CPU), keyboard IRQ
- **User Mode**: Ring-3 entry via iretq, ELF64 loader, 16 int 0x80 syscalls (write, exit, yield, open, close, read, lseek, stat, readdir, spawn, wait, getpid, kill, thread_create, thread_join, thread_exit), validated user pointers (`access_ok`/`copy_from_user`/`copy_to_user`)
- **Processes & SMP**: real `struct thread`/`struct process` split (schedulable unit vs. resource owner), multi-core bring-up (AP INIT-SIPI-SIPI, per-CPU LAPIC/GDT/TSS), a single global scheduler lock held across context switches, per-process address spaces + guarded kernel stacks + fd tables, timer-preemptive priority-band scheduler with aging, wait queues, uncatchable kill, real blocking `sys_wait`, ring-3 process AND thread handles (kernel kthreads and joinable ring-3 threads sharing one address space), an interactive shell that's itself a real process (see Phase 17–20)

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
- **A tenth bug, found by re-reading this same work rather than by a crash,
  closed immediately after**: `schedule()` calls made from syscall context
  (`sys_exit`/`sys_yield`) ran with IF=1 (needed for Bug 7's disk-I/O fix),
  so a timer IRQ could land mid-`schedule()` and re-enter it from the timer
  ISR while the outer call was still mutating scheduler state — a real
  reentrancy hazard, not just a Phase D/SMP hypothetical. **Fixed**:
  `schedule()` now `cli`s at entry (same `pushfq`/`cli`/conditional-`sti`
  idiom `cpu/spinlock.c` already uses) and restores the caller's original
  interrupt state on every path that doesn't go through `kernel_ctx_switch`
  (which always resumes with IF=1 anyway, per Bug 6). Verified by rerunning
  `test sched roundrobin`/`kill`/`reap`/`stackguard` in QEMU — all still
  pass, including the two that most directly exercise this exact shape
  (kthreads calling `process_exit_self`, i.e. IF=1, while being actively
  preempted). Full writeup: `docs/architecture/process-and-scheduling.md`
  §8(a)/Bug 10, §16.

### Phase 19 — Priority Scheduling, Real Blocking Wait, Ring-3 Handles, Interactive Shell ✅
Closes Phases C and D of the scheduler roadmap and turns the kernel's own
shell into a real process. Full detail and postmortems:
`docs/architecture/process-and-scheduling.md` (§4.2, §6.2/§6.3, §7.4, §16
Bugs 11–12) and `TODO.md`.
- **Priority scheduling**: 4 fixed bands (REALTIME/INTERACTIVE/NORMAL/
  BACKGROUND), round-robin within a band, aging (200ms/band — deliberately
  short; a "nicer" multi-second period would let a busy high-priority
  child visibly freeze even the kernel's own NORMAL-priority shell for
  several seconds before rescuing it). Verified via `test sched priority`.
- **Real blocking `process_wait()`/`sys_wait`**: replaces the earlier
  busy-poll. `struct process` gained `parent`/`parent_pid` (the latter
  guards against a recycled PCB slot aliasing an unrelated new process),
  `child_list`/`child_next` (live children, for `ps`), and `zombie_head`/
  `zombie_next` (a parent's exited-but-unclaimed children — deliberately
  two separate fields; one double-duty field would corrupt a sibling chain
  the moment a process tree goes more than one level deep). Verified via
  `test sched wait`.
- **Ring-3 process handles** (`docs/ARCHITECTURE.md` §3.4/§3.5): `sys_spawn`
  now returns a capability handle, not the raw pid; `sys_wait`/`sys_kill`
  (new) resolve a handle back to a pid via each process's own
  `handles[PROC_HANDLE_MAX]` table. Closes the confused-deputy gap a raw
  pid left open. Verified via a temporary `user/init.c` scaffold covering
  the full handle lifecycle, including invalid- and reused-handle
  rejection.
- **Interactive process control**: `main.c`'s old single-hardcoded-process
  auto-launch (`process_start_first()`, one-way) is gone; the shell now
  calls `process_adopt_current()` to become a real, permanently-scheduled
  process itself, with `run`/`ps`/`kill`/`wait`/`nice` commands. Verified
  interactively in QEMU (monitor-injected keystrokes) against a real
  `/init.elf`: spawn, list, block-and-collect a real exit code, and
  no-op-safe kill/wait on already-exited or nonexistent pids.
- **Two more bugs found and fixed getting here** (full postmortems:
  architecture doc §16, Bugs 11–12): (11) `schedule()`'s zombie hand-off
  ran *after* the "nothing else runnable" early return, which deadlocked
  the very first real use of blocking `process_wait()` (a parent sitting
  `BLOCKED`, not READY/RUNNING, with nothing else in the table) — fixed by
  reordering. (12) `struct process::parent` is a raw pointer into a
  recycled PCB slot; checking only `state != PROCESS_UNUSED` could
  misidentify an unrelated new process as the still-alive original parent
  — fixed by also snapshotting and comparing `parent_pid`.
- **Also fixed along the way**: main.c's process-launch removal (done
  before this phase) had left a stray unconditional inner `for(;;) hlt`
  that trapped the shell after at most one keystroke; removed as part of
  wiring up the real interactive loop.

### Phase 20 — SMP, Thread/Process Split, Ring-3 Threads ✅
Full spec, comparative analysis, the complete bug ledger, and every design
decision below: `docs/architecture/process-and-scheduling.md` (its own
Phase 4/5, §4.1/§6/§13/§16).
- **Per-CPU foundation** (`kernel/cpu/percpu.h/.c`): `struct cpu_data` per
  core (own TSS + RSP0/#DF stacks, `current_thread`, `pending_*_reap`,
  `online`), indexed by an APIC-ID→index table built from the MADT;
  `this_cpu()` resolves via `lapic_get_id()`. GDT split into
  `gdt_init_bsp()` (shared descriptors) + `gdt_init_this_cpu()` (per-core
  TSS descriptor + `ltr`).
- **AP bring-up** (`kernel/cpu/smp.c`, `ap_trampoline.asm`/`ap_entry.asm`):
  real-mode AP trampoline relocated below 1MB, INIT-SIPI-SIPI sequencing,
  each AP lands in `ap_main()` and becomes a real, permanently-scheduled
  idle thread — not a busy-loop stub.
- **`struct thread`/`struct process` split** (`process.h`, full rewrite):
  `thread_table[MAX_THREADS=256]` (the schedulable unit — context, kernel
  stack, state, priority, running/pinned CPU) separated from
  `process_table[MAX_PROCESSES=64]` (the resource owner — pid, address
  space, parent/child/zombie tracking, fd/handle tables).
  `current_thread` is a real per-CPU field (`cpu_table[]`); `current_process`
  is a derived, read-only `current_thread->proc` macro — every external
  consumer outside `process.c` needed either zero changes or a one-line
  NULL-check fix. `thread_create(proc, entry)` lets one process own more
  than one kernel thread, sharing its address space.
- **Single global scheduler lock** (`g_sched_lock`), held **across the
  context switch itself** — released only by whichever thread resumes on
  the far side. Per-CPU run queues are explicitly deferred (§8/§17): the
  shipped design is "one lock first, measure before sharding," not a
  speculative build. Every SMP-bring-up crash found during this phase was
  some variant of releasing this lock even one instruction too early.
- **Ring-3 threads** (Phase 5 of the arch doc): `thread_create_user()`/
  `thread_join()`/`thread_exit_self()` plus `sys_thread_create`/
  `sys_thread_join`/`sys_thread_exit` give a ring-3 process real
  multi-threading — an additional thread sharing the SAME address space,
  entering ring 3 directly, with its own dedicated (deterministically
  placed) user stack. Joinable, not auto-reaped like a kthread.
  `MAX_PROCESSES`/`MAX_THREADS` raised 16/16 → 64/256 for this (catching
  and fixing `KSTACK_SIZE`'s accidental coupling to `MAX_PROCESSES` in the
  same pass).
- **Bugs found and fixed**: 14 during SMP bring-up (two CPUs believing
  they ran the same PCB, from two different root causes; the scheduler
  lock's release-point being the load-bearing fix), zero during the
  thread/process split itself (credited to the split being designed
  around the two hazards SMP had already taught), zero new kernel bugs
  during ring-3 threads (one real test-flakiness finding in an existing
  selftest's sample size, not a kernel bug — see the arch doc §12/§13).
  Full ledger: arch doc §16.
- **Ten selftests**: `test sched roundrobin`/`kill`/`reap`/`stackguard`/
  `wait`/`priority` (pre-existing, still green under `-smp 4`),
  `test smp sched`/`kill` (cross-core dispatch + kill-while-running-
  elsewhere), `test thread smp`/`exit` (shared address space across
  threads, reaped only when the LAST thread exits), `test ring3 threads`
  (a real ring-3 process spawning/joining a second thread of itself,
  needs `make run-embkfs` for `/init.elf`).

### Phase 21 — EMBKFS v2: Compression, Encryption, Snapshots, OS-Native Features ✅
Full spec (byte-exact structs, every design decision, all known
limitations stated plainly): `docs/EMBKFS_spec_v2.2.md`. Beginner-friendly
guide also published. Plan executed in one extended pass with standing
autonomous-execution authorization; every phase has its own permanent
selftest, all green in `test embkfs all`.
- **Real timestamps**: CMOS RTC driver (`kernel/drivers/rtc.c`, from
  scratch, BCD/binary + 12/24h handling, Hinnant's `days_from_civil`);
  wired through every inode-mutating call site. Found and fixed four
  pre-existing gaps where a parent directory's own timestamps were never
  updated on create/rename/unlink/link.
- **Multi-volume mounting**: `embkfs_init()` mounts every EMBKFS
  superblock it finds (not just the first), each on its own VFS mount
  point. **Found and fixed a real, previously-invisible bug getting this
  verified**: the ATA driver only ever routed the primary IDE channel's
  interrupt (IRQ14) and always used its Bus-Master DMA registers — any
  I/O to a 3rd/4th drive (secondary channel) hung for ~2.7 hours before
  timing out (indistinguishable from a dead hang against any real
  timeout). Fixed: per-channel IRQ handling (IRQ14 + IRQ15) and per-
  channel Bus-Master register bases.
- **Crypto primitives** (`kernel/crypto/`): SHA-256, HMAC-SHA256, PBKDF2,
  AES-256, AES-256-XTS, all from scratch, all checked against externally-
  computed known-answer vectors (not just round-trip self-consistency).
- **Per-extent compression**: an LZ4-inspired (not byte-exact LZ4) codec;
  only kept when it actually shrinks block usage by at least one block.
- **AES-256-XTS encryption**: mount-time passphrase prompt (masked echo),
  PBKDF2 key derivation, LUKS-style key-check, per-block deterministic
  XTS tweak (the block's own disk address — no stored nonce needed).
  Cross-verified against an independent Python `cryptography`-based
  AES-XTS implementation, never reusing the kernel's own crypto code.
- **OS-native features** (all four, as scoped up front): self-healing
  dual-superblock repair; instant (O(1)) snapshots with a snapshot-aware
  allocator (`snap create/list/delete/rollback`) — found and fixed a real
  ordering bug where a snapshot's own creation commit could free the
  blocks its frozen root needed; process-provenance (`writer_pid` in
  every inode, `stat <path>` to view it); an HMAC-SHA256 verified-root
  boot check (kernel-embedded key — explicitly scoped as authentication,
  not real asymmetric signing, and documented as such).
- **Oracle-first discipline held throughout**: `mkfs_embkfs.py`/
  `verify_embkfs.py` updated alongside every on-disk format change,
  including independent Python ports of the compression decoder and an
  XTS decrypt path.
- **Known, deliberately-scoped limitations** (all documented in the spec,
  not hidden): snapshot allocator hold-back is conservative, not true
  per-block refcounting; rolling back to a snapshot reverts the snapshot
  registry too (newer snapshots become inaccessible); verified-root uses
  one shared kernel-embedded key; no ciphertext stealing in XTS (never
  needed, every write is block-rounded).

---

## Major To-Do Buckets (Rough Priority)

Full detail lives in `TODO.md`, organized by subsystem. Rough priority order:

### High Priority (block real programs)
1. ~~User-pointer validation~~ — ✅ done, Phase 18.
2. ~~Process & Scheduling gaps (preemption, blocking, uncatchable kill,
   per-process fd tables)~~ — ✅ done, Phase 18. Priority scheduling and
   real blocking wait also now done, Phase 19. SMP + the thread/process
   split + ring-3 threads also now done, Phase 20.
3. ~~File I/O syscalls~~ — ✅ done, Phase 18.
4. ~~A real blocking `sys_wait`~~ — ✅ done, Phase 19: `process::parent`/
   `parent_pid`/`zombie_head`/`zombie_next`/`child_wait` tracking, no more
   busy-polling. Found and fixed a real deadlock getting this exercised
   for the first time (Bug 11).
5. ~~The syscall-context `schedule()` reentrancy gap~~ — ✅ done, Phase 18
   (Bug 10 above).
6. ~~Ring-3 process handles for spawn/wait/kill~~ — ✅ done, Phase 19: closes
   the confused-deputy gap a raw pid argument left open.
7. ~~SMP~~ — ✅ done, Phase 20: real multi-core bring-up, per-CPU
   `current_thread`, one global scheduler lock held across context
   switches. Per-CPU run queues specifically remain deferred (see Lower
   Priority below) until the global lock is measured to bottleneck.

### Medium Priority (real-world use)
1. **Filesystems**: ~~EMBKFS crash-safe orphan reclaim~~ (mount-time sweep
   done; on-disk orphan list for the crash-safety tier still open), ~~VFS
   multi-mount~~ (done, Phase 21), `.truncate`/`.readlink` ops still open;
   FAT32 IDE secondary channel note is now moot (both channels work, see
   Phase 21). EMBKFS v2 (Phase 21) shipped compression/encryption/
   snapshots/self-heal/provenance/verified-boot — see `TODO.md`'s new
   EMBKFS v2 subsection for that work's own follow-ups (snapshot
   refcounting, asymmetric verified-root signing).
2. **Drivers**: keyboard modifiers/extended scancodes, `vmm_map_mmio_wc`
   (write-combining), USB isochronous transfers, xHCI hub support (legacy
   HCs got hub support in Phase 18; xHCI's is a separate code path, still
   open), USB hot-plug.
3. **Synchronization**: per-CPU heap caches, locks around currently-unlocked
   global tables (per-process fd tables, EMBKFS open-ref table) — SMP now
   exists (Phase 20) but nothing currently races these specific tables
   from more than one core concurrently; revisit before that changes. The
   process/thread tables themselves are already locked (`g_sched_lock`,
   Phase 20).

### Lower Priority (architecture)
1. **Per-CPU run queues**: the one piece of the SMP work deliberately left
   unbuilt — see Phase 20 above and the arch doc §8/§17 for why (measure
   the global lock first, don't shard speculatively).
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
- VFS now supports real multi-mount (longest-prefix match, `vfs_find_mount`,
  up to `VFS_MAX_MOUNTS = 8`) — landed alongside EMBKFS v2's multi-volume
  mounting (Phase 21). No `.readlink`/`.truncate` ops yet (see `TODO.md`).
- EMBKFS v2 (Phase 21) known, deliberately-scoped limitations — all
  documented in `docs/EMBKFS_spec_v2.2.md`, not hidden: snapshot allocator
  hold-back is conservative (not true per-block refcounting); rolling
  back to a snapshot reverts its own registry entry too (newer snapshots
  become inaccessible after an older rollback); verified-root boot check
  uses one kernel-embedded HMAC key, not real asymmetric signing; no XTS
  ciphertext stealing (never needed — every write is block-rounded).

### Drivers
- VBE now does EDID + BIOS mode-list enumeration instead of one hardcoded
  mode — only reached when no GPU driver claims the display (Phase 16
  covers the common case) — but the fallback-of-fallbacks (no VBE at all,
  text mode at 0xB8000) is unchanged and still hangs at `.vbe_failed`.
- USB: hub support on UHCI/OHCI/EHCI (Phase 18); xHCI hub support and
  isochronous transfers on any controller are still open.
- Keyboard: ASCII-only, no modifiers/extended scancodes.
- ATA: both IDE channels now fully interrupt-driven and DMA-capable
  (Phase 21 fixed the secondary channel — see Phase 21 above); AHCI is
  still port-0-only.

### User Mode / Process
- User pointers are now validated (`access_ok`/`copy_from_user`/
  `copy_to_user`, Phase 18).
- 16 syscalls exist (write, exit, yield, open, close, read, lseek, stat,
  readdir, spawn, wait, getpid, kill, thread_create, thread_join,
  thread_exit) — `sys_spawn`/`sys_wait`/`sys_kill` take/return capability
  handles (Phase 19), not raw pids.
- Preemption, wait queues, the uncatchable kill, priority scheduling
  (4 bands + aging), real blocking `process_wait()`/`sys_wait`, SMP
  (multi-core bring-up, a real per-CPU `current_thread`, one global
  scheduler lock held across context switches), the `struct thread`/
  `struct process` split, and joinable ring-3 threads are all built
  (Phases 18–20). `schedule()` is reentrancy-safe against the timer ISR
  regardless of which context it's called from, and its zombie hand-off
  no longer deadlocks a blocked waiter. Still open: per-CPU run queues
  (§6.3/§8 of the arch doc) — deliberately deferred until the single
  global scheduler lock is *measured* to bottleneck, not built ahead of
  need.

---

## Test Status

| Subsystem | Status | Notes |
|-----------|--------|-------|
| PMM | ✅ Passes | Allocates/frees pages consistently |
| VMM | ✅ Passes | Maps kernel + user, NX works |
| EMBKFS v2 | ✅ Passes | `test embkfs all` — create/read/write/unlink/mkdir/rename/link/symlink, collision-chain and B-tree-descent regressions, RTC timestamps, multi-volume mount, LZ compression, self-heal, snapshots, process-provenance, HMAC verified-root boot check. `test crypto all` — SHA-256/HMAC/PBKDF2/AES-256/XTS against known-answer vectors. Independent oracle: `verify_embkfs.py` (incl. `--passphrase` for encrypted images) |
| FAT32 | ✅ Passes | Read/write long filenames on MBR partition, oracle-validated against `fsck.vfat`/`mcopy` |
| VFS | ✅ Passes | Path resolution, real multi-mount (longest-prefix), multi-FS discovery, `ls`, boot selftest (6 checks) |
| File descriptors | ✅ Passes | open/close/read/write/seek/fstat, create-on-open, unlink-while-open, selftest |
| ATA | ✅ Passes | DMA+IRQ on BOTH IDE channels (Phase 21 fixed the secondary channel), MBR partition discovery |
| AHCI | ✅ Passes | Read/write on port 0 (polling), verified against host ground truth |
| Ring-3 Entry | ✅ Passes | ELF loads, user code runs, iretq works, syscalls dispatch |
| SMP / Threads | ✅ Passes | `test smp sched`/`kill`, `test thread smp`/`exit`, `test ring3 threads` (Phase 20) — cross-core dispatch, shared-address-space threading, joinable ring-3 threads, all under `-smp 4` |
| LAPIC Timer | ✅ Passes | 100 Hz ticks, TSC calibrated |
| rwlock | ✅ Passes | Reader/writer locking, smoke test passes |
| GPU / Framebuffer | ✅ Passes | `test gpu` (`fb_run_selftests`): fill_rect/get_pixel/copy_rect round-trip, exact colors, boundary non-bleed. Plus manual: Bochs DISPI + VirtIO-GPU modeset verified via QEMU screendump; VBE EDID/enumeration fallback verified (selects a genuinely different mode than the old hardcoded one under `-vga cirrus`). |
| USB (UHCI/OHCI/EHCI/xHCI) | ✅ Passes | `test usb` (`usb_run_selftests`): cross-checks discovered controllers against a fresh PCI rescan. Plus manual: HID keyboard input + mass-storage block device verified per-HC in QEMU, an EHCI+UHCI companion handoff, and a keyboard behind a `usb-hub` on UHCI (hub support, Phase 18). |
| Process lifecycle / scheduler | ✅ Passes | `test sched roundrobin`/`kill`/`reap`/`stackguard`/`wait`/`priority` (`process_test_*`, Phase 18/19) — six selftests, all green. Plus manual: `/init.elf` loads, runs in ring 3 on a guarded kernel stack, exits cleanly, zero exceptions; real preemption verified interleaving two kthreads; the full ring-3 handle lifecycle (`sys_spawn`/`sys_wait`/invalid/reused-handle rejection) verified end to end; the interactive shell's `run`/`ps`/`kill`/`wait`/`nice` commands verified end to end in QEMU via monitor-injected keystrokes against a real spawned/waited process. |
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
behind each item. Everything through Phase 21 (user-pointer validation,
scheduler Phases B/C/D, file I/O syscalls, real blocking `sys_wait`,
ring-3 process handles, per-process fd tables, the interactive shell, SMP +
the thread/process split + ring-3 threads, and EMBKFS v2's full feature
set) is now done; what's left before self-hosting:

1. **libc port** (newlib first) — implement syscall backends, don't write
   one from scratch — the hinge to running existing software. The syscall
   surface (16 calls, file I/O + process/thread management, all handle/
   capability-safe) is now broad enough to make this the natural next step.
2. **Native shell/coreutils**, built on top of the kernel's own interactive
   process-control commands (`run`/`ps`/`kill`/`wait`/`nice`) rather than
   from scratch, then a self-hosting toolchain (tcc first), then mouse +
   compositor + GUI. See `docs/ARCHITECTURE.md` §5 for the full
   critical-path roadmap to self-hosting.
3. **Per-CPU run queues** — the one piece of the SMP work deliberately left
   unbuilt: the single global `g_sched_lock` is the shipped design, and
   per-CPU queues are scoped as the *next* step only once that lock is
   *measured* to bottleneck (see the arch doc §8/§17), not built ahead of
   real need.
4. **True per-block snapshot refcounting / asymmetric verified-root
   signing** — both EMBKFS v2 known limitations (Phase 21 above), flagged
   in `docs/EMBKFS_spec_v2.2.md` as natural v2.x follow-ups, not attempted
   in this pass to keep the crypto/allocator surface reviewable.

---

## Build Environment

- OS: Ubuntu Linux. Toolchain: x86_64-elf cross compiler at `/usr/local/cross/bin`.
- NASM, QEMU, GNU Make, VS Code, GDB (`docs/GDB_CHEATSHEET.md`).
- Repository: `github.com/teo1747/EmbLinkOs`.

## Build / Run Commands

```bash
make                     # build everything
make run                 # boot in QEMU (serial → stdio)
make run-smp             # boot with -smp 4 (real multi-core)
make run-ahci            # boot with an extra AHCI SATA disk attached
make run-embkfs          # boot with a flat EMBKFS image as sdb
make run-embkfs-tree     # boot with a 2-level EMBKFS tree image as sdb (has /init.elf)
make run-embkfs-encrypted # boot with an ENCRYPTED EMBKFS volume (passphrase: correcthorsebattery)
make run-multivol        # boot with TWO EMBKFS volumes (sdb -> "/", sdc -> "/sdc")
make run-usb-embkfs      # boot with an EMBKFS volume mounted over xHCI USB mass storage
make run-vga-std         # Bochs DISPI (QEMU -vga std)
make run-virtio-gpu      # VirtIO-GPU accelerated scan-out (QEMU -vga virtio)
make run-usb-uhci        # UHCI + usb-kbd + usb-storage
make run-usb-ohci        # OHCI + usb-kbd + usb-storage
make run-usb-ehci        # EHCI + usb-storage (high speed)
make run-usb-xhci        # xHCI + usb-kbd
make debug               # GDB server on :1234 (paused)
make clean               # remove binaries (preserves disk.img / ahci.img)
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
