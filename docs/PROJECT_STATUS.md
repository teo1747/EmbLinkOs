# EmbLinkOS — Project Status & Handoff

*Last updated: 2026-07-13. Living document — reconciled from two previously
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
- **User Mode**: Ring-3 entry via iretq, ELF64 loader (static **and dynamic linking**, in-kernel — no userspace `ld.so`), ~48 int 0x80 syscalls (file I/O, process/thread management, window/compositor, IPC, memory, `sleep_ms`/`proc_alive`/`win_resize` among the newest), validated user pointers (`access_ok`/`copy_from_user`/`copy_to_user`)
- **Processes & SMP**: real `struct thread`/`struct process` split (schedulable unit vs. resource owner), multi-core bring-up (AP INIT-SIPI-SIPI, per-CPU LAPIC/GDT/TSS), a single global scheduler lock held across context switches, per-process address spaces + guarded kernel stacks + fd tables, timer-preemptive priority-band scheduler with aging, wait queues, uncatchable kill, real blocking `sys_wait`, ring-3 process AND thread handles (kernel kthreads and joinable ring-3 threads sharing one address space)
- **Userland & UI**: a newlib libc port (freestanding / static / dynamically-linked build modes), a window compositor (z-order, app-owned chrome, zero-copy windows, resizable windows, desktop widgets), and **EmUI** — a from-scratch, SwiftUI-flavored UI toolkit apps are built against, with an app-runtime layer (`EM_APPLICATION`/`EM_WIDGET`) that removes almost all boilerplate. The OS boots by default into a graphical home launcher (app grid + a live desktop clock widget), not the interactive kernel shell (see Phase 22).

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
limitations stated plainly): `docs/EMBKFS_spec_v2.2.md`, then
`docs/EMBKFS_spec_v2.3.md` for what landed after it (atomic rename, bounded
extents, the ~140x read-path rebuild). Beginner-friendly
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

### Phase 22 — newlib userland, GUI stack (compositor + EmUI), dynamic linking, mouse, IPC

*Landed ahead of the "self-hosting first" ordering ARCHITECTURE.md originally
sketched — see that doc's §5/§10 for the reasoning. This phase covers
several previously-untracked pieces of work in one entry since they shipped
together and depend on each other; individually they span a mouse driver,
an in-kernel window compositor, a from-scratch UI toolkit built in five
successive versions (V1 → V5), an in-kernel dynamic linker, and a
newlib-based libc port.*

- **newlib port**: freestanding, static-newlib, and dynamically-linked build
  modes (`user/lib/crt0.c`, `syscalls.c` — the POSIX retargeting layer over
  the raw `embk_syscallN` ABI). The stock cross-toolchain's newlib lacked
  C99 printf formats (`%z`/`%ll` compiled out); fixed with a from-source
  newlib rebuild into a separate prefix (`NEWLIB_PREFIX`) rather than
  patching the toolchain in place. See `user/README.md` and
  `docs/BUILD_SETUP.md`.
- **In-kernel dynamic linker** (`kernel/arch/x86_64/syscall/elf.c`): no
  userspace `ld.so`, no `PT_INTERP` — the kernel loads an `ET_EXEC` app plus
  its one `DT_NEEDED` shared object (`libembk.so`, the UI toolkit compiled
  `-fPIC`) into the same address space at `exec`/`spawn` time, applying
  `RELATIVE`/`64`/`GLOB_DAT`/`JUMP_SLOT` relocations eagerly (no lazy PLT)
  plus `R_X86_64_COPY` for the first-ever cross-object data (not function)
  reference, which needed dedicated handling to bind back to the app's own
  copy of the symbol rather than one living in the `.so`. Two-way symbol
  resolution: the app's toolkit calls resolve into `libembk.so`; the `.so`'s
  libc calls resolve back into the app's own `--export-dynamic`'d static
  newlib (newlib itself is not PIC, so it can't live in the `.so`).
- **Mouse driver** (`kernel/drivers/input/mouse.c`) + IntelliMouse wheel
  support, feeding the compositor's pointer routing.
- **Window compositor** (`kernel/gfx/compositor.c`): z-ordered windows,
  kernel-drawn chrome (title bar + close button) or fully app-owned
  ("chromeless") chrome, zero-copy shared-memory windows (the client renders
  directly into pages mapped both into its own address space and the
  compositor's), window move/resize, click-to-focus, title-bar drag, a
  click-latch so a press landing while an app is mid-render isn't silently
  dropped (`sys_win_input` is a state poll, not an event queue), desktop
  widgets in their own z-band (always above the wallpaper, always below app
  windows), and pointer capture (drag routing sticks to the press-owner
  window even once the cursor leaves its bounds, needed for fast/steep
  drags to survive the poll rate).
- **EmUI** (`ui/`): a from-scratch, SwiftUI-flavored declarative UI toolkit,
  built in layers — a scene-graph render IR (`ui/scene`), a flexbox layout
  engine (`ui/layout`), a general reactive core (`ui/reactive`), an
  immediate-mode API over a retained instance tree with automatic
  reconciliation (`ui/declare`), a design-token theme system (`ui/theme`),
  a themed widget kit (`ui/kit`), a CPU render backend with a from-scratch
  TrueType rasterizer (`ui/backend`), and a DSL (`ui/dsl/em.{h,c}`) that
  progressed through seven iterations: V1/V2 (brace-scoped containers,
  chainable modifiers), V3 (components, navigation, charts/lists), V4
  (app-owned "chromeless" window chrome, a larger modern component set —
  Dropdown/Toast/Spinner/Gauge/SearchField/Disclosure/StatCard/EmptyState,
  tab and split-view navigation), V5 (resizable windows via a corner
  grip, always-on-desktop widgets via `EM_WIDGET`), V6 (a **menu system** —
  a `MenuBar` of drop-down `Menu`s and a right-click `ContextMenu`, both in
  the overlay layer with outside-click dismiss; right-button input is plumbed
  through the app runtime), and V7 (a focusable, scrolling, multi-line
  `TextEditor` with full caret navigation — the kernel keyboard driver gained
  extended-scancode arrow/Home/End/Delete keys, delivered as `EMBK_KEY_*`
  codes, to support it). The backend also got a **performance pass** this
  cycle (see `EMUI_INTERNALS.md`): table-based gamma text blending (~5× on
  the text path, replacing 3 per-pixel Newton-loop `sqrtf`s) and an integer
  premultiplied source-over fast path in `draw_image`. The toolkit also gained
  its first signature **material** — frosted **glass** (`.glass = 1` / the
  `Glass()` container): the pre-existing backdrop-blur render primitive, now
  surfaced in the DSL with a translucent accent-tinted fill and an edge
  highlight, and applied to the V6 menus by default so the UI reads as layered
  rather than a flat Windows/Linux clone. Glass also went **compositor-deep**:
  an `Acrylic` window (`EMBK_WINF_GLASS`) has the kernel compositor blur the
  *desktop* behind it and composite the window over it (`fb_blur_region` +
  `fb_blit_uniform` in the framebuffer driver), so the title bar frosts the
  wallpaper and other windows show through — a real desktop-showthrough effect,
  not just an in-window blur. The desktop itself became an **aurora** — a soft
  mesh-gradient of overlapping color blobs (`aurora_build` in the compositor)
  shown through the home launcher's translucent scrim, so the acrylic windows
  now blur a colorful field and the frosted glass genuinely glows. A V4.1 app-runtime
  layer (`ui/dsl/em_app.c`, `EM_APPLICATION`/`EM_WIDGET`) collapses what
  used to be ~150 lines of per-app boilerplate (font loading, arena setup,
  window creation, the event loop, dirty-rect presenting, teardown) into
  one declarative struct literal, and gates the whole event loop on
  **retained updates** — a frame is only built when there's an input edge,
  a structural change, a timer tick, or a component explicitly requests one
  (an idle app does zero UI work per poll). Every layer below the DSL has a
  host-compiled, host-run unit test (`make scene-test`/`backend-test`/
  `font-test`/`layout-test`/`reactive-test`/`declare-test`, no QEMU); whole
  screens render to PNG via `make showcase`/`showcase-v2` for a fast visual
  check before booting a VM. See `docs/EMUI_GUIDE.md` (how to build an app)
  and `docs/EMUI_INTERNALS.md` (how the toolkit itself works).
- **IPC** (`kernel/ipc/`): capability handles, channels, and endpoints for
  cross-process communication, underlying window/resource handles more
  broadly, not just a message-passing feature on its own.
- **`epfs`** (`kernel/fs/epfs.c`): an additional lightweight filesystem
  backend alongside EMBKFS/FAT32 in the VFS mount registry.
- **The home launcher** (`user/bin/home.c`) replaces the interactive kernel
  shell as the default boot target: a full-screen chromeless desktop window
  hosting an app-tile grid (click to `spawn()`), spawning the clock desktop
  widget (`clockw.elf`) automatically. It hand-rolls its own event loop
  (predates/bypasses the `EM_APPLICATION` runtime, since it owns the
  desktop layer rather than an app window) and tracks each spawned child by
  its **spawn handle** (opaque, capability-scoped — not a raw pid), which
  surfaced a real bug worth flagging for anyone spawning children from an
  EmUI app: failing to `wait()` a dead child leaks its handle out of the
  16-per-process handle table, and once that table fills, the *next* spawn
  call fails and kills the child it just created.
- **Reference apps**: `uidemo.elf`/`wmdemo.elf` (V1–V3 toolkit + compositor
  demos), `v4demo.elf` (the fullest reference — chromeless chrome, tabs,
  split view, every V4/V5 component), `v6demo.elf` (the V6 menu system — the
  "Menus" home tile), `v7demo.elf` (the V7 text editor — the "Editor" home
  tile), `clockw.elf` (the minimal `EM_WIDGET` reference, an uptime clock
  ticking on the desktop). New EmUI apps under `user/bin/*.c` are
  auto-discovered by the Makefile and packed into the EMBKFS image — no
  per-app build rule or mkfs entry to add by hand.

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
behind each item. Everything through Phase 22 (user-pointer validation,
the full scheduler + SMP + thread/process split + ring-3 threads, EMBKFS
v2's full feature set, **and** the newlib port, dynamic linking, the window
compositor, and the EmUI GUI stack — all of which landed *ahead of* the
self-hosting-first ordering originally sketched in `docs/ARCHITECTURE.md`)
is now done — **and so is self-hosting for C, now with real headers**: TCC
compiles, assembles, links and runs `#include`-using programs against the
on-image newlib headers and `libc.a` (`test tcc real` → `exit=42`, byte-exact
buffered-stdio output), and the OS has **rebuilt one of its own tools from
source** — `tally`, four translation units compiled on-OS, linked in a
separate invocation (the compile-then-link shape every build tool assumes),
installed, and A/B-verified against the host-built original through the
shell's live extern pipeline (`test tcc tally`). The OS also hosts
C++/libstdc++, CPython 3.14 and git 2.49; see [PORTS.md](PORTS.md).

**Honest scope of that claim:** the OS can compile, run, and now *rebuild its
own static C programs*. It cannot yet rebuild *itself* wholesale: that needs a
build orchestrator (decided: a native structured build tool — targets as typed
records, recipes as spawn argvs, staleness by content hash — with a make port
deferred to the future ports/compat story; see TODO.md), and the excluded
classes are TCC facts, stated plainly — no `__thread` (TCC reads no
`newlib.ld`/PT_TLS), no `libembk.so`-linked apps, no C++, and the kernel build
wants GCC (whose driver fork/execs `cc1`/`as`/`ld`, which this OS structurally
cannot do). "Self-hosting for C" remains the accurate phrase.

What's left:

1. **Native shell/coreutils** — the structured shell has shipped
   ([SHELL.md](SHELL.md)); coreutils and a real TTY remain. The GUI stack
   landing first means a native *editor* is now also plausible as an EmUI
   app before a text shell exists — worth reconsidering that ordering. See
   `docs/ARCHITECTURE.md` §5 for the current critical-path roadmap.
2. **A real TTY** — scrollback, line editing — for whichever of the above
   needs a text console; the framebuffer console today is output-only.
3. **Per-CPU run queues** — the one piece of the SMP work deliberately left
   unbuilt: the single global `g_sched_lock` is the shipped design, and
   per-CPU queues are scoped as the *next* step only once that lock is
   *measured* to bottleneck (see the arch doc §8/§17), not built ahead of
   real need.
4. **Lazy PLT binding** for the dynamic linker — relocations are currently
   applied eagerly at load time (see Phase 22); fine at today's app count
   and sizes, worth revisiting if process start latency becomes a problem.
5. **True per-block snapshot refcounting / asymmetric verified-root
   signing** — both EMBKFS v2 known limitations (Phase 21), flagged
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
make embkfs.img          # build every userland app + libembk.so, pack an EMBKFS image
make run-embkfs-cow      # boot a pristine EMBKFS image, then grade the post-boot copy
make run-ui              # boot to a shell; `run /uidemo.elf` launches the EmUI toolkit live
make run-wm              # boot to the window-compositor demo (two composited windows)
make scene-test backend-test font-test layout-test reactive-test declare-test  # host-run UI unit tests, no QEMU
make showcase-v2         # render EmUI DSL/V4/V5 screens to PNG (needs Pillow) -- see docs/EMUI_GUIDE.md
make debug               # GDB server on :1234 (paused)
make clean               # remove binaries (preserves disk.img / ahci.img)
```
See `docs/BUILD_SETUP.md` for first-time toolchain/newlib setup and
`CONTRIBUTING.md` for the complete, currently-maintained target list.

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
