# EmbLink OS — Known Issues & Improvements

Open items only, grouped by subsystem. Completed work lives in
PROJECT_STATUS.md (and git history). Keep this file to what's actually
left to do.

---

## Bootloader

### Stage 1
- [ ] Hardcoded Stage 2 sector count (8 sectors) — should be dynamic.

### Stage 2
- [ ] Loads a fixed 512 sectors for the kernel regardless of actual size.
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
- [x] Uses VMM_NOCACHE (PCD) — correct for register MMIO.
- [ ] Add vmm_map_mmio_wc() for write-combining (framebuffer, large buffers).
  Requires PAT setup in IA32_PAT (MSR 0x277); then PWT + PAT bits in the PTE
  select the WC slot. See Intel SDM Vol 3, §11.12.
- [ ] No deallocation — bump allocator only. Need vmm_unmap_mmio that frees
  the virtual range (requires tracking allocated ranges: list or tree).
- [ ] No bounds check on mmio_next_virt — could run off the end of the MMIO
  region silently. Add MMIO_END and check before advancing.

### Heap
- [x] kmalloc/kfree locking — spinlock with cli/sti save-restore.
- [ ] No slab allocator — add fixed-size pools (16/32/64/…) on top of the
  linked list for O(1) common-case alloc.
- [ ] krealloc always copies — could expand in place if the next block is free.
- [ ] First-fit is O(n) — consider segregated free lists or best-fit.
- [ ] Security hardening (later): guard pages between allocations, allocation
  randomization, quarantine/delayed-free for use-after-free detection.

### Synchronization
- [ ] Spinlock has no deadlock detection / lock-ordering checks.
- [ ] No reader-writer locks (all mutual exclusion is exclusive).
- [ ] Per-CPU heap caches to reduce lock contention (multi-core, future).

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
- [ ] Migrate to TSC + HPET for precise time (HPET one-shot, TSC high-res),
  both discovered via ACPI.
- [ ] Tick handler only does counter++ — eventually: preemption check +
  scheduler invocation (once there are processes).

---

## Storage

### ATA / DMA
- [ ] PRD_EOT has a stray trailing semicolon (`#define PRD_EOT 0x8000;`) in
  ata.c. Works in the current single-assignment use but will break inside a
  larger expression — remove the semicolon.
- [ ] Multi-PRD scatter-gather for buffers > 64KB or spanning pages (current
  limit: single contiguous region ≤ 64KB / ≤ 128 sectors).
- [ ] LBA48 DMA (READ DMA EXT 0x25) for > 128 sectors / disks > 128GB on the
  IDE/DMA path. (AHCI path already does LBA48.)
- [ ] DMA buffer must be kernel BSS/contiguous (KV2P). For arbitrary virtual
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
- [ ] `%llu` used in out-of-range error messages — verify kprintf handles it
  (the path wasn't exercised yet); else cast to uint64_t / unsigned int.
- [ ] No partition support — devices are whole disks (sda, sdb…). Partition
  naming convention (sdaN) needed once we parse partition tables / MBR/GPT.
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
- [ ] VBE mode hardcoded to 0x118 (1024×768×24). Proper selection:
  1. EDID query (INT 10h AX=4F15h BL=01h), parse detailed timing descriptor 1
     for native resolution; fall back gracefully if unavailable.
  2. Enumerate VBE modes (VbeModeListPtr in VBEInfoBlock @ 0x0E; 0xFFFF-
     terminated uint16 array; call 4F01h per mode for ModeInfoBlock).
  3. Filter + score: require supported + graphics + LFB + direct-color
     (MemoryModel 6); score by closeness to native; prefer 32 > 24 > 16 bpp.
  4. Fallback chain: 1920×1080 → 1280×1024 → 1024×768 → 800×600 → 640×480,
     final fallback text mode at 0xB8000.
  - Refs: VBE 3.0 (Function 15h DDC), EDID 1.4 (VESA E-EDID).
- [ ] UEFI GOP path (modern alternative to VBE; needs the UEFI bootloader).
- [ ] Eventually: GPU acceleration — find GPU via PCI, GPU driver, 2D accel
  (rectangle blit, glyph cache).

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
- [ ] Migrate to USB HID once there's a USB stack.

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