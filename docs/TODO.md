# EmbLink OS — Known Issues & Improvements

Open items only, grouped by subsystem. Completed work lives in
PROJECT_STATUS.md (and git history). Keep this file to what's actually
left to do.

---

## Bootloader

### Stage 1
- [x] Hardcoded Stage 2 sector count (8 sectors) — should be dynamic.

### Stage 2
- [x] Loads a fixed 512 sectors for the kernel regardless of actual size.
  - LESSON (learned the hard way): when the kernel outgrew the old 90-sector
    limit, the tail wasn't loaded. Symptoms were truncated MMIO addresses and
    garbled E820 — looked like a pointer bug, was actually missing code/data
    in RAM. The Makefile now warns if the kernel ELF exceeds 512 sectors.
  - PROPER FIX (bootloader v2): read the ELF header in real mode, parse the
    PT_LOAD segments (max p_offset + p_filesz), load exactly the sectors
    needed. Do this alongside the UEFI rework.
- [ ] Hardcoded kernel LBA start (sector 9) — works only because we control
  the disk layout. A real OS finds the kernel via a filesystem.
- [ ] Disk image must be padded (truncate) — workaround for reading past the
  actual kernel data. Goes away with ELF-aware loading.

### General
- [ ] No USB/CD boot — hard disk only.
- [ ] No error recovery on failed disk reads (just halts).
- [ ] BIOS only — no UEFI support.

---

## Memory Management

### PMM
- [ ] Linear scan for a free page is O(n) — slow under heavy allocation.
  Future: free list or buddy allocator.
- [ ] Stack region hardcoded at 0x200000 — should be allocated dynamically
  and tracked properly.
- [ ] Stack and PMM bitmap could collide if the stack grows > 1MB.
  - Move the stack to its own dedicated region, or allocate it via PMM after
    init, with proper guard pages.

### VMM — kernel mapping extent (NEW — today's near-miss)
- [ ] vmm_init maps only the first 2MB of the kernel into the kernel range
  (`for phys = 0; phys < 0x200000`). The kernel currently fits (~1.5MB), but
  the moment it crosses 2MB (more drivers, filesystem code, bigger BSS) the
  high addresses become unmapped → page fault on first access.
  - FIX: map dynamically up to a rounded-up KV2P(kernel_end) instead of a
    hardcoded 0x200000.

### VMM — page-table location limit (measured)
- [x] Boots fine at 4 GB. Page tables (~28 KB) + bitmap (160 KB) fit under
  physical 2 MB, so KP2V works.
- [ ] HARD LIMIT ~32 GB RAM: the PMM bitmap alone (~1 MB at 32 GB) plus page
  tables pushes allocations past physical 2 MB, where KP2V (kernel mapping is
  0–2 MB only) faults. 128 GB → ~4 MB bitmap → guaranteed crash.
  - FIX (when needed): two-phase paging bootstrap.
    * Phase 1: pre-map the first ~16 MB into the kernel range (bootloader),
      or build a minimal direct map covering where page tables will live.
    * Phase 2: build the full direct map using P2V (direct-map access).
    * Then switch VMM table access from KP2V to P2V.

### vmm_map_mmio

- [ ] Add vmm_map_mmio_wc() for write-combining (framebuffer, large buffers).
  Requires PAT setup in IA32_PAT (MSR 0x277); then PWT + PAT bits in the PTE
  select the WC slot. See Intel SDM Vol 3, §11.12.
- [ ] No deallocation — bump allocator only. Need vmm_unmap_mmio that frees
  the virtual range (requires tracking allocated ranges: list or tree).
- [ ] No bounds check on mmio_next_virt — could run off the end of the MMIO
  region silently. Add MMIO_END and check before advancing.

### Heap
- [x] kmalloc/kfree locking — spinlock with cli/sti save-restore.
- [ ] Slab allocator — fixed-size pools (16/32/64/128/256/512/1024 bytes) with O(1)
  common-case alloc (DEFERRED: current design has metadata collision issues; needs
  safer approach like tracking slab block ranges separately).
- [x] krealloc in-place expansion — checks if next block is free before allocating
  new; coalesces and returns same pointer if expansion succeeds. Reduces copies
  and fragmentation in dynamic array growth (e.g., embkfs_free_index_reserve).
- [ ] First-fit is O(n) — consider segregated free lists by size class (lower priority).
- [ ] Security hardening (later): guard pages between allocations, allocation
  randomization, quarantine/delayed-free for use-after-free detection.

### Synchronization
- [ ] Spinlock has no deadlock detection / lock-ordering checks.
- [x] Reader-writer locks — rwlock_t in kernel/cpu/rwlock.c (many readers OR one
  writer, single atomic state word; irqsave/irqrestore since multiple readers
  can't share one saved-flags slot). Smoke test: `test rwlock`. Not yet wired
  into any data structure (read-mostly candidates arrive with SMP).
  - [ ] Reader-preferring — a steady reader stream can starve a writer. Add a
    writer-preferring variant (pending-writer flag blocking new readers) if
    needed.
- [ ] Per-CPU heap caches to reduce lock contention (multi-core, future).

### Memory Management
- [x] EFER.NXE enabled (in gdt_init) so PTE bit 63 (NX) is legal — required
  before any VMM_NX mapping. Was missing; surfaced as a reserved-bit #PF
  (error 0x0C) the first time W^X set NX on the data segment. Consider moving
  the enable into vmm_init/CPU-feature init where it belongs logically.

---

## Interrupts & Timers

### APIC / IRQ
- [ ] MADT interrupt source overrides not applied to IO-APIC routing
  (e.g. timer IRQ0 → GSI2). Only keyboard (GSI1, no override) is routed today.
  When routing more IRQs, consult overrides + polarity/trigger flags.
- [ ] Spurious-interrupt vector (0xFF) is set in the SVR but has no IDT
  handler — add a no-op handler to be safe.
- [ ] Spurious IRQ 7 / IRQ 15 (legacy PIC) detection — read the PIC ISR and
  check the bit before EOI. (Low priority now that the PIC is masked/retired.)
- [ ] No per-CPU LAPIC init — needed when APs come online (SMP).
- [ ] LAPIC timer not using TSC-deadline mode (more precise, modern).
- [ ] Duplicated register save/restore between irq_common and
  irq_common_lapic in isr.asm — refactor to share.
- [ ] irq_register/irq_unregister don't save/restore IRQ mask state.

### Exceptions
- [x] Page-fault handler prints CR2 + decodes the error code (P/W/U/RSVD/I)
  per Intel SDM Vol 3A §4.7.

### Timer / Time
- [x] Migrate to TSC + HPET for precise time (HPET one-shot, TSC high-res),
  both discovered via ACPI.
- [ ] Tick handler only does counter++ — eventually: preemption check +
  scheduler invocation (once there are processes).

---

## Storage

### ATA / DMA
- [x] PRD_EOT had a stray trailing semicolon (`#define PRD_EOT 0x8000;`) in
  ata.c. Works in the current single-assignment use but will break inside a
  larger expression — remove the semicolon.
- [ ] Multi-PRD scatter-gather for buffers > 64KB or spanning pages (current
  limit: single contiguous region ≤ 64KB / ≤ 128 sectors).
- [ ] LBA48 DMA (READ DMA EXT 0x25) for > 128 sectors / disks > 128GB on the
  IDE/DMA path. (AHCI path already does LBA48.)
- [x] DMA buffer no longer has to be contiguous in callers: ATA DMA now uses
  direct multi-PRD for physically contiguous mappings and a bounce-buffer
  fallback for arbitrary virtual buffers.
  For arbitrary virtual
  buffers: walk pages + multi-PRD, or bounce-buffer.
- [ ] Secondary channel DMA (offset 0x08 in BMIDE) not handled.
- [ ] No cache-coherency handling (fine on QEMU/x86; matters on some HW).

### AHCI
- [ ] Polls PxCI for completion — switch to interrupt-driven (PxIS + the
  controller's interrupt) for efficiency.
- [ ] Single PRD only — add multi-PRD scatter-gather.
- [ ] Only port 0 / first device exercised — generalize across all present
  ports if multiple SATA disks appear.

### Block layer
- [x] Partition support — MBR (DOS) table parsed; each primary partition is
  exposed as a child block device (sda1, sda2…) that delegates to its parent
  disk at the partition's start LBA, bounded by its length. Mount probe sees
  partitions alongside whole disks. See kernel/block/partition.c.
  - [ ] GPT not parsed yet — a protective-MBR (type 0xEE) disk is detected and
    skipped with a notice.
  - [ ] Extended/logical partitions (type 0x05/0x0F) detected but not walked.
  - [ ] Scan must run after `sti` (ATA read path is IRQ-driven). Placed in
    main.c right before block enumeration / mount probe for that reason.
- [ ] Block-layer reads/writes on IDE secondary channel hang — DMA + IRQ
  are only wired for the primary channel (IRQ 14, primary BMIDE base).
  Secondary needs IRQ 15 + BMIDE offset 0x08 + per-channel state. Until
  then, mountable disks must be on IDE primary or AHCI. (FAT32 test disk
  is on IDE primary slave = sdb for this reason.)
- [ ] Block-layer DMA bounce buffer is shared global state — needs a lock
  once transfers can be concurrent (IRQ-driven / multi-threaded). Fine while
  synchronous. Also: bounce always copies; multi-PRD scatter-gather (page-walk
  via vmm_get_phys) would be the zero-copy alternative (already on TODO).

---

## PCI

- [ ] BARs parsed but not stored in the pci_device struct — only printed.
  Cache them so drivers retrieve without re-reading.
- [ ] No ECAM/MCFG (PCIe memory-mapped config) — only legacy CAM. Parse the
  MCFG ACPI table, map config space, support 4KB extended config.
- [ ] No recursive bridge scanning — brute force works but doesn't follow
  secondary buses behind PCI-to-PCI bridges properly.
- [ ] No capability-list parsing (MSI/MSI-X, power management).
- [ ] No interrupt routing — wire device INTx/MSI to IO-APIC/LAPIC.
- [ ] No device-specific driver-binding mechanism yet.
- [ ] Vendor/device ID → human-name database (currently only class names).

---

## Drivers — Display / Console

### Framebuffer / VBE
- [x] ~~Real-mode VBE path hardcoded to mode 0x118~~ — done: `stage2.asm`'s
  `vbe_init` now does EDID query (INT 10h AX=4F15h BL=01h, Detailed Timing
  Descriptor #1 for native resolution) → exact-match search through the
  enumerated VBE mode list (`VideoModePtr` at VBE Info Block offset 0x0E,
  `find_mode_for_resolution` requiring ModeAttributes
  {supported, graphics, LFB} and MemoryModel 6/direct-color, preferring
  higher bpp among matches) → a fixed fallback list (1920×1080 → 1280×1024
  → 1024×768 → 800×600 → 640×480) → the original hardcoded mode 0x118 if
  literally nothing else worked. `selected_mode` defaults to 0x118 and is
  only ever overwritten by a mode `find_mode_for_resolution` actually
  confirmed exists, so any bug in the new EDID/enumeration path degrades to
  exactly the old behavior rather than something worse — deliberate, since
  stage2 has no serial debugging to fall back on if real-mode BIOS calls
  misbehave. Verified in QEMU with `-vga cirrus` (a device neither
  `bochs_vbe.c` nor `virtio_gpu.c` claims, so this path actually runs):
  picked 1280×1024×16bpp from the fallback list (Cirrus doesn't support
  32/24bpp direct-color there), rendering verified correct via screendump.
  Remaining: no scoring by "closest to native" when an EXACT resolution
  match isn't found (only the fixed fallback list is tried in that case);
  no final text-mode-at-0xB8000 fallback if VBE itself is entirely absent
  (still `.vbe_failed` → hang, unchanged from before).
  - Refs: VBE 3.0 (Function 15h DDC), EDID 1.4 (VESA E-EDID).
- [ ] UEFI GOP path (modern alternative to VBE; needs the UEFI bootloader).
- [x] ~~GPU acceleration~~ — done: `gpu.c` probes PCI for a GPU driver before
  `fb_init`; `bochs_vbe.c` does runtime DISPI modeset, `virtio_gpu.c` drives
  an accelerated guest-memory scan-out (TRANSFER_TO_HOST_2D + RESOURCE_FLUSH
  per dirty rect). `framebuffer.c` gained a RAM backbuffer with dirty-rect
  `fb_present()`, clipped fill/copy rects, lines, circles, alpha-blit.
  Remaining: `virtio_gpu.c` only negotiates VIRTIO_F_VERSION_1 (no indirect
  descriptors, no multiple scanouts, no 3D/Virgl); `bochs_vbe.c` has no
  `-vga qxl` support; no write-combining on the LFB mapping
  (`vmm_map_mmio_wc` still doesn't exist — see `vmm_map_mmio` entry above).

### Keyboard
- [ ] Only ASCII press events. Add release tracking for proper modifier state.
- [ ] No modifier handling (Shift, Caps Lock, Ctrl, Alt) — needs a state
  machine; Shift+key, Caps Lock toggle (latches differently from Shift).
- [ ] No extended scan codes (0xE0): arrows, Home/End/PgUp/PgDn, Ins/Del,
  right-side Ctrl/Alt, Windows/Menu — multi-byte scancode state machine.
- [ ] No F1–F12 (0x3B–0x44).
- [ ] No layout abstraction — only US QWERTY hardcoded; support keymaps
  (AZERTY, Dvorak, …).
- [ ] No PS/2 controller (8042) init — relies on BIOS leaving it usable.
  Configure via port 0x64: disable mouse port, set scancode set, enable IRQs.
- [x] ~~Migrate to USB HID once there's a USB stack~~ — done, see USB section.

---

## USB Host Controllers

All four generations now bring up their root ports and enumerate devices:
`xhci.c` (own IRQ-driven path, HID + mass storage), `ehci.c` (async
QH/qTD schedule, periodic interrupt QHs, releases FS/LS ports to a
companion), `uhci.c` (I/O BAR4, frame-list schedule), `ohci.c` (MMIO,
ED/TD lists + HCCA periodic table). `usb_core.c` is the shared HCD-agnostic
layer: enumeration, a HID boot-keyboard driver (with shift map), a
mass-storage BOT/SCSI driver that registers block devices, and hub support
(below). Legacy HCs are polled from the main loop (`usb_poll`); xHCI stays
interrupt-driven.

- [x] ~~No hub support on the legacy HCs~~ — done for UHCI/OHCI/EHCI:
  `usb_core.c`'s `usb_hub_attach` fetches the class-specific hub descriptor
  (port count), then for each port does power-on + reset + status read
  (mirroring each HCD's own root-port scan, just through class-specific hub
  requests instead of native port registers) and enumerates whatever's
  connected — single level only, with a depth guard against a malformed/
  looping topology. Verified in QEMU: a keyboard behind a `usb-hub` on UHCI
  enumerates and works (`addr 2`, boot keyboard ready). Deliberately NOT
  covered: xHCI (separate legacy code path, not `usb_core.c` — still just
  logs "USB Hub detected" and stops, matching its pre-existing "only the
  first device enumerates" limitation) and full/low-speed devices behind a
  high-speed EHCI hub (needs a Transaction Translator, EHCI spec §4.14 — a
  genuinely separate, larger feature; only same-speed downstream devices
  are enumerated). Both are documented gaps, not silent ones.
- [ ] No isochronous transfers (audio/video class devices unsupported).
- [ ] No USB3/SuperSpeed on xHCI beyond what already worked before this
  change — streams, bursting, and the SuperSpeedPlus descriptors are unused.
- [ ] Static per-controller-type tables (`UHCI_MAX_HC`/`OHCI_MAX_HC`/
  `EHCI_MAX_HC` = 2, `USB_MAX_DEVICES` = 16) — fine for QEMU testing, will
  need to become dynamic for real hardware with many devices.
- [ ] Root ports (and now hub ports) are scanned once at boot; no hot-plug
  (connect-status-change interrupt) handling anywhere in the USB stack.
- [ ] EHCI/UHCI/OHCI control and bulk transfers are synchronous busy-polls
  (bounded by a spin-count timeout, not wall-clock) — fine for boot-time
  enumeration, would stall the kernel if called after multitasking exists.
  (Multitasking now exists — see Process & Scheduling below — but nothing
  currently calls into USB from more than one process at a time, so this
  hasn't bitten yet; revisit before that changes.)
- [x] ~~No automated selftest for the display or USB stack~~ — partially
  done: `test usb` (`usb_run_selftests`, `usb.c`) cross-checks the USB
  controller table against a fresh PCI class 0x0C/subclass 0x03 scan and
  asserts every discovered controller was classified into a real HC kind
  and (if it has a usable BAR) actually initialized — deliberately
  independent of which HC/device happens to be attached, a real assertion
  about the discovery/classification code path itself. `test gpu`
  (`fb_run_selftests`, `framebuffer.c`) draws known primitives
  (`fb_fill_rect`, `fb_copy_rect`) and asserts exact colors read back via
  `fb_get_pixel`, exercising the actual color pack/unpack path for whatever
  mode is active. Neither covers live HID/mass-storage data transfer or
  actual on-screen/on-host rendering — those are still manual-QEMU-only
  (screendumps + serial-log inspection per HC generation, done this
  session for UHCI/OHCI/EHCI/xHCI and Bochs DISPI/VirtIO-GPU).

---

## Filesystem

### EMBKFS
- [ ] Crash while a file is unlinked-but-open LEAKS its inode. Deferred-free
  keeps the object alive (links==0, no dirent) until last close, but the
  open-ref table is in-memory/per-boot — a crash in that window loses the
  reclaim. Non-corrupting (no fd survives reboot to dangle), just lost space.
  - FIX: mount-time sweep — scan for inodes with links==0 and free them
    (orphan reclaim). Cheap, runs once at mount.
  - STRONGER (later): an on-disk orphan list for crash-safe deferred delete,
    replayed on mount. Bigger; only if crash-safety of unlinked-open matters.
- [ ] Open-ref table (`g_open_refs`, EMBKFS_MAX_OPEN_OBJECTS = 64) is a fixed
  array with linear scan and NO lock. Fine single-core/synchronous; needs a
  lock once opens can race (SMP / preemption) — same class as the block-layer
  bounce buffer.
- [ ] `embkfs_parent_dir_oid` resolves `..` by a full-tree scan (O(tree) per
  `..`). VFS now does dot-dot itself with a breadcrumb stack, so this path is
  only hit by EMBKFS-internal callers — but if those stay, a stored parent
  back-ref would make it O(1). Low priority.

### VFS
- [ ] Single mount only. `vfs_find_mount` ignores `path` and returns the one
  used slot. Real multi-mount = longest-prefix match (deepest mount point that
  prefixes the path) + hand back the path remainder relative to that mount.
  The op table and resolve loop are already ready; only this function changes.
- [ ] No `.readlink` op. Symlinks resolve to the LINK vnode but can't be
  followed; EMBKFS already has `embkfs_readlink_object`.
  - FIX: add `.readlink` to vfs_ops + adapter, follow links inside
    `vfs_resolve` with a hop-count bound (ELOOP). NOTE: true `..` then differs
    from the lexical breadcrumb `..` (a symlink's real parent vs its path
    parent) — revisit the stack semantics when this lands.
- [ ] No `.truncate` op → O_TRUNC can't be honored at open(). EMBKFS has
  `embkfs_truncate_object`.
  - FIX: add `.truncate` op + adapter; wire O_TRUNC in vfs_open.
- [ ] stat() simplifications: directory nlink hardcoded to 2 (real = 2 +
  subdir count); directory size reported as ENTRY COUNT, not bytes (ls tags it
  'e'). Decide whether anything depends on either before "fixing."
- [ ] `unlink` op has remove(3) semantics — it rmdirs an empty directory.
  POSIX unlink(2) must return EISDIR on a directory; rmdir(2) is dirs-only.
  - FIX (syscall layer): split unlink(2)/rmdir(2), either via a separate
    `.rmdir` op or a stat-and-dispatch in the syscall handler.
- [ ] Absolute paths only — no cwd / relative resolution (arrives with
  per-process context).
- [ ] `vfs_mount` duplicate-mount check is a raw strcmp on `at` (not a
  normalized path). Fine for the single "/" mount.

### File descriptors
- [x] ~~fd table is fixed (64), global, per-boot. No per-process fd tables~~ —
  done: `struct fd_entry` moved to `kernel/fs/fd.h` (public) and `struct
  process` now embeds `struct fd_entry fds[FD_MAX_OPEN]`. `fd.c` picks the
  table via a `fd_table()` helper — `current_process->fds` if there is a
  current process, else a boot-time-only `g_boot_fds` (preserves the
  existing pre-process `test ring3`/selftest behavior that runs before any
  process exists). Verified via QEMU: two spawned processes opening
  different files see independent fd numbering/state, no cross-talk.
  Open-file-description sharing (fork / dup / dup2, shared cursor across
  dup'd fds) is still NOT modeled — each fd entry still owns its own
  cursor; only isolation between processes was added, not fd aliasing
  within one.
- [ ] g_boot_fds / per-process fds arrays are unlocked mutable state — needs
  a lock once syscalls from the same process can race (e.g. real SMP; today
  preemption exists but a single process only runs one syscall at a time).
- [ ] O_TRUNC reserved but not honored (blocked on the VFS truncate op above).
- [ ] O_APPEND is implemented by re-stat-to-end before each write — one extra
  stat per write, and a TOCTOU window if writes can race. Acceptable while
  single-threaded; revisit with concurrency.

---

## User Mode & Syscalls

### ELF Loading (COMPLETED)
- [x] ELF64 loader (kernel/cpu/elf.c/h) loads PT_LOAD segments with correct
  permissions (PF_X → no NX, PF_W → writable). Maps pages in user VA space
  (0x0000400000000000–0x0000700000000000 low-half), restores real p_flags
  after loading (NX on data, exec on code).
- [x] Entry point (e_entry) from ELF header used instead of hardcoded
  0x400000. init_blob is now an ELF-format binary, not flat .bin.
- [x] Context save/restore (kernel_ctx_save / kcontext) allows kernel to
  resume after user program exits via sys_exit.

### Ring-3 Entry & Exit (COMPLETED)
- [x] iretq path to ring 3 with user code/data selectors, user RSP, EFLAGS=0x202
  (IF+reserved). Works with interrupt handling + IRQ re-enable on exit.
- [x] sys_exit (syscall 2) restores saved kernel context → control returns to
  enter_user_mode (no halt).
- [x] Interrupts live during user execution (no CLI mask); IRQs pre-enabled
  before entering ring 3.

### Security — user-pointer validation (HIGH, the real hole) — DONE
- [x] ~~`sys_write` dereferences a raw user pointer with NO validation~~ — done:
  `kernel/cpu/usercopy.c/h` adds `access_ok(ptr, len)` (checks the range is
  below the canonical low-half boundary `0x0000800000000000` and every page
  in it is actually mapped in `current_process->pml4_phys` via
  `vmm_get_phys_in`), plus `copy_from_user`/`copy_to_user` (access_ok +
  memcpy, `-EMBK_EFAULT` on failure) and `copy_string_from_user` (bounded,
  byte-at-a-time, for path arguments). Wired into every syscall that takes a
  user buffer or path (`sys_write`, `sys_read`, `sys_open`, `sys_stat`,
  `sys_readdir`, `sys_spawn`). Verified with a temporary test in
  `user/init.c`: a normal buffer round-trips correctly, and a deliberately
  bad pointer (a kernel address) is rejected with `-EMBK_EFAULT` instead of
  being dereferenced — confirmed via QEMU, then the test scaffold was
  reverted.

### Userspace loader
- [x] elf_load partial-load cleanup now happens by loading into a fresh
  address space and destroying that address space on load failure.
  but segment N+1's frame alloc fails, segment N's pages stay mapped and
  nothing unmaps them. Harmless in the single shared address space today;
  becomes a real leak once per-process address spaces exist (a failed load
  must discard the whole attempt). FIX: unwind mapped segments on error, or
  build into a fresh address space that's simply dropped on failure.
- [x] Loads /init.elf from the filesystem (multi-block extent read verified).
  Embedded-blob path removed.

### Syscall transport
- [x] `int 0x80` (software gate) implemented: dispatch table now has 12 calls
  — write=1, exit=2, yield=3, open=4, close=5, read=6, lseek=7, stat=8,
  readdir=9, spawn=10, wait=11, getpid=12.
- [ ] Fast path `syscall`/`sysret` deferred: needs STAR/LSTAR/SFMASK MSRs + EFER.SCE,
  AND swapgs + a per-CPU GS base for the kernel-stack switch (syscall does NOT
  switch stacks via the TSS). Wants per-CPU/SMP infra. GDT already laid out user
  data-before-code for it.
- [x] ~~Syscall table: wire the fd/VFS syscalls~~ — done: `sys_open`,
  `sys_close`, `sys_read`, `sys_lseek`, `sys_stat`, `sys_readdir` (via a
  callback-based `sys_readdir_cb` walking `vfs_readdir`, filling a
  `struct sys_dirent { ino, type, name[59] }` per entry) all go straight
  through to the existing `vfs_*`/`vfs_fd_*` layer, guarded by
  `copy_from_user`/`copy_to_user`/`copy_string_from_user` (see
  user-pointer validation above).
- [x] ~~`sys_write` is serial-only and ignores fd~~ — done: fd==1 still goes
  straight to serial (no fd-table round trip needed for the common case);
  any other fd routes through `vfs_fd_write` via a bounce buffer.
- [x] Found bug (not on this list originally): `sys_exit` read the exit code
  from `r->rax` (the syscall number — always 2) instead of `r->rdi` (the
  actual argument), so every process's exit code was hardcoded to 2
  regardless of what it passed. Fixed to read `r->rdi`; verified exit code
  changes correctly (6 for `/init.elf`'s counter+bss-sum check).
- [x] Found bug (not on this list originally): `int 0x80` is an interrupt
  gate, so IF is auto-cleared for the entire syscall handler — any syscall
  that waits on a hardware completion IRQ (e.g. disk I/O inside `sys_open`)
  hung forever. Fixed by adding `sti` as the first instruction of
  `syscall_dispatch()`.

### Ring-3 / TSS plumbing
- [x] Ring-3 entry point: enter_user_mode() loads ELF, maps user stack,
  saves kernel context, enters ring 3 via iretq.
- [x] ~~`tss_set_rsp0` exists but RSP0 is a single static kernel stack~~ — done:
  `schedule()`/`process_start_first()` call `tss_set_rsp0(next->kstack_top)`
  before every switch. See "Process & Scheduling" below for the subsystem
  this now belongs to.

### Per-process address spaces
- [x] vmm_destroy_address_space walks the user half, frees mapped frames and
  user page-table pages, and is called on ELF load failure, stack setup
  failure, and normal process exit.
- [x] Per-process PML4 (kernel half aliased 256/511, user half private);
  vmm_map_in/get_phys_in; late CR3 switch; higher-half p_vaddr rejected.

---

## Process & Scheduling

Full phased spec, comparative analysis (Linux/Windows/BSD/XNU), and every bug
below in detail: `docs/architecture/process-and-scheduling.md`. Current
mechanism: a static `MAX_PROCESSES = 16` PCB table, timer-driven preemptive
round-robin (100Hz LAPIC tick), wait queues for blocking, an uncatchable
`process_kill`, and per-process fd tables. `process_create()` builds a fresh
address space + page-mapped guarded kernel stack from an ELF path and
fabricates a first context that lands in `process_trampoline`, which
`iretq`s to ring 3. Phase B (see roadmap in the architecture doc) is now
substantially complete.

- [x] ~~Ring-3 entry `#GP` on every process start~~ — `process_trampoline`'s
  inline-asm `iretq` frame pushed the literal `$2` instead of the `%2`
  operand (the real `0x23` selector) for CS. Loading CS from the resulting
  null-descriptor selector faulted immediately. Fixed.
- [x] ~~`kstack_top` truncated to 16 bits~~ — `struct process::kstack_top` was
  `uint16_t` but stored a 64-bit heap address; `tss_set_rsp0()` read the
  truncated field. Would have corrupted RSP0 on the first ring-3 interrupt.
  Now `uint64_t`. Fixed.
- [x] ~~Zombie processes never reclaimed~~ — nothing transitioned
  `PROCESS_ZOMBIE` back to `PROCESS_UNUSED`; after exactly `MAX_PROCESSES`
  exits, `process_create` would fail forever. Fixed: `process_reap()`, deferred
  one `schedule()` call behind the actual exit (can't free a stack still being
  executed on). Not yet exercised in practice — `main.c` only ever creates one
  process today, so the deferred-reap path has never actually fired; needs
  either a second process or the `test sched reap` selftest below to prove it.
- [x] ~~No kernel-stack guard page~~ — `alloc_kernel_stack` was a flat
  `kmalloc`, silently corrupting whatever sat next to it on overflow. Fixed:
  `vmm_alloc_kernel_stack`/`vmm_free_kernel_stack` page-map the stack with an
  unmapped guard page directly below it.
- [x] ~~Kernel-stack region invisible in the first process's own page
  tables~~ — introduced *while fixing* the guard-page bug above: the new
  region was placed in a PML4 slot untouched before boot.
  `vmm_create_address_space()` shares the kernel half by copying PML4 entries
  *by value* at creation time, not by live reference — a slot that's
  not-present at that moment stays not-present in that process forever, even
  after the kernel's own table fills it in moments later. `#PF` on first
  touch, which can't even push its own fault frame (same region backs
  `TSS.RSP0`) → `#DF`. Fixed by reusing `MMIO_BASE`'s already-populated PML4
  slot (256 GiB in, clear of the real MMIO bump allocator) instead of a fresh
  one.
- [x] ~~No preemption~~ — done: `lapic_timer_handler` (`lapic.c`) now calls
  `schedule()` after `lapic_send_eoi()`, at the existing 100Hz tick rate.
  Genuine timer-driven round-robin, verified via `test sched roundrobin`
  (two kthreads interleave without either calling `sys_yield`/`sys_exit`).
- [x] ~~No blocking / wait queues~~ — done: `struct wait_queue { struct
  process *head; }` plus an intrusive `wait_next` link on `struct process`.
  `wait_queue_block()` marks the caller `PROCESS_BLOCKED`, links it in, and
  calls `schedule()`; `wait_queue_wake_one`/`wait_queue_wake_all` unlink and
  mark `PROCESS_READY`. Verified via `test sched roundrobin`'s blocking
  variant.
- [ ] No priority — strict round-robin, no bands, no aging/anti-starvation.
- [x] ~~No uncatchable kernel-level kill~~ — done: `process_kill(pid)`
  (`process.c`) forces `PROCESS_ZOMBIE` regardless of current state, unlinks
  the target from any wait queue it's blocked on, and reaps it immediately
  unless it's the currently-running process (in which case it defers via
  the existing `g_pending_reap` mechanism, same as a normal exit). Matches
  `docs/ARCHITECTURE.md` §3.3's requirement that a process which never
  cooperates can still be stopped. Verified via `test sched kill`.
- [x] ~~No per-process file descriptor table~~ — done, see "File
  descriptors" under Filesystem above (`struct process::fds`, `fd_table()`
  helper in `fs/fd.c`).
- [x] ~~No `sys_wait`/`sys_spawn` syscalls~~ — done: `sys_spawn` calls
  `process_create()` on a user-supplied path (via `copy_string_from_user`);
  `sys_wait` busy-polls (`process_find` + `sys_yield`) until the target
  hits `PROCESS_ZOMBIE`, then `process_reap()`s it and returns its exit
  code. `sys_yield` is now wired to syscall number 3. Also added
  `sys_getpid`. Verified via a temporary `user/init.c` scaffold: a parent
  spawns a child, waits, and observes the child's real exit code; reverted
  after verification.
- [x] ~~`process_create`/`proc_alloc` both increment `next_pid`~~ — fixed:
  removed the redundant `proc->pid = next_pid++` from `process_create()`;
  `proc_alloc()` is now the sole assigner.
- [x] ~~No automated selftest~~ — done: `test sched roundrobin`, `test sched
  kill`, `test sched reap`, `test sched stackguard` (`process_test_*` in
  `process.c`, wired into `selftests.c`), matching the four selftests
  `docs/architecture/process-and-scheduling.md` §12 specifies. `reap` in
  particular exercises the deferred-reap path noted above, which nothing
  had actually triggered before (main.c previously only ever created one
  process).
- [ ] Single core only — `current_process` is one global pointer, not
  per-CPU; no locking around the process table (fine until SMP or
  preemption reentrancy exist — see the spec's Synchronization section for
  what changes then). NOTE: preemption reentrancy now DOES exist
  (timer-driven `schedule()` above) — this note is closer to load-bearing
  than when it was written; the process table itself has held up so far
  only because everything that mutates it currently runs with a single
  logical CPU and interrupts are the only source of reentrancy (handled by
  each ISR being non-reentrant, not by an explicit lock).

### New bugs found while building preemption/syscalls/spawn (not on this list originally)
- [x] **RFLAGS.IF corruption in context switch.** `kernel_ctx_switch`/
  `kernel_ctx_save` (`kcontext.asm`) capture RFLAGS via `pushfq`/`pop rax`
  and are always called from inside an interrupt-gate ISR (timer IRQ,
  `int 0x80`), which auto-clears IF on entry — so the *live* flags always
  read IF=0 at the capture point, regardless of the outgoing process's true
  state. Saving that corrupted snapshot meant every resumed process came
  back with interrupts permanently masked, hanging after its first
  preemption. Found via `test sched roundrobin` (scheduler hung after one
  cycle). Fixed by forcing `or rax, 0x200` (set IF) before storing to the
  RFLAGS field in both functions — safe because reaching that save point at
  all proves the process had IF=1 moments earlier (a maskable IRQ can't
  fire with IF=0, and ring-3 can't execute cli/sti, it's privileged and
  would #GP).
- [x] **`int 0x80` leaves IF=0 for the whole syscall.** Same root cause as
  above, different symptom: any syscall that blocks on a hardware
  completion IRQ (disk I/O inside `sys_open`/`sys_read`/`sys_spawn`'s ELF
  load) hung forever because the IRQ that would satisfy it could never
  fire. Found while verifying file I/O syscalls. Fixed with `sti` as the
  first instruction of `syscall_dispatch()`.
- [x] **`proc_alloc()` race between allocation and initialization.** Found
  while verifying `sys_spawn`: with real preemption now active, a second
  `process_create()` call (from a live syscall context) could be preempted
  mid-setup during its slow ELF-load disk I/O — but `proc_alloc()` had
  already marked the new slot `PROCESS_READY` immediately, so the scheduler
  could pick a half-initialized PCB (no pml4/kstack/ctx yet) and crash.
  Fixed: `proc_alloc()` now marks the slot `PROCESS_BLOCKED` instead;
  `process_create()`'s own final step is the only place that sets
  `PROCESS_READY`, after everything is actually built.
- [x] **PCB leak on `process_create()` error paths.** Found via code review
  while fixing the race above: none of the four early-return failure paths
  (address-space creation, ELF load, user-stack alloc, kernel-stack alloc)
  reset `proc->state` back to `PROCESS_UNUSED`, so a failed `process_create`
  permanently leaked the PCB slot. Fixed by adding the reset to all four
  paths.

---

## Core / Library

- [ ] Refactor done: kprintf + snprintf share one format_string core. Future:
  add %b (binary) / field-precision if needed; both wrappers benefit.
- [ ] kstring: only the subset in use is implemented. Add more (strstr, strtok,
  memchr, etc.) as needed — don't pre-build the whole libc.

---

## Architecture (big-ticket, later)

- [ ] SMP — multi-core bring-up. The CPU list is already available from the
  MADT; needs per-CPU LAPIC init, AP startup (INIT-SIPI-SIPI), per-CPU data.
- [ ] Portability / HAL discipline — keep new upper-layer code arch-neutral
  (no inb/outb, no direct page-table pokes, no x86 asm in logic); route
  arch-specific operations through arch_* interfaces. Real ARM64 port is a
  later dedicated campaign — don't pre-abstract against a single architecture.