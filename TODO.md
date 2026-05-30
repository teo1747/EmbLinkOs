# Helios - Known Issues & Improvements

## Bootloader

### Stage 1
- [ ] Hardcoded sector count (8 sectors) for Stage 2 - should be dynamic

### Stage 2
- [ ] Loads fixed 90 sectors for kernel regardless of actual size
  - Better: read ELF header first, then read exactly needed sectors
- [ ] Disk image must be padded to 1MB via truncate
  - Workaround for reading past actual kernel data
- [ ] Hardcoded LBA start (sector 9) - works because we control the disk layout
  - Real OS would use filesystem to find kernel


- [~] Loads fixed 512 sectors for kernel (was 90, outgrew it once)
  - LESSON: when kernel exceeded 90 sectors, the tail wasn't loaded;
    symptoms were truncated MMIO addrs + garbled E820 (looked like a
    pointer bug, was actually missing code/data in RAM)
  - Makefile now warns if kernel ELF > 512 sectors
  - PROPER FIX (bootloader v2): read ELF header in real mode, parse
    PT_LOAD segments (max p_offset+p_filesz), load exact sectors.
    Do this alongside UEFI rework.

### General
- [ ] No support for booting from USB/CD - hard disk only
- [ ] No error recovery on failed disk reads (just halts)
- [ ] BIOS-only - no UEFI support

## Memory Management

### PMM
- [ ] Stack region hardcoded at 0x200000
  - Should be allocated dynamically and tracked properly
- [ ] No support for memory hot-plugging (e.g., PCI devices)
- [ ] No support for memory overcommit (e.g., swap)
- [ ] No support for memory encryption
- [ ] Linear scan for free page is O(n) - slow with lots of allocations
  - Future: free list or buddy allocator

### Memory Layout
- [ ] Stack and PMM heap could collide if stack grows >1MB
  - Move stack to its own dedicated region (e.g., 0x90000 area)
  - Or allocate stack via PMM after init
  - Mark proper guard pages around stack

### VMM - Direct Map Limitation

- [ ] After full direct map: also implement vmalloc-style dynamic mapping
- [ ] Note: storage (HDD/SSD) is NOT a VMM concern - handled by drivers

### vmm_map_mmio
- [ ] MMIO pages mapped as cached memory — slow for framebuffer
  - Need to set cache-disable bit (VMM_NOCACHE) for general MMIO
  - For framebuffer specifically, want write-combining (WC) via PAT
  - See Intel SDM Vol 3, section 11.12 (Memory Type Range Registers + PAT)
- [ ] No deallocation — bump allocator only
  - Eventually need vmm_unmap_mmio that frees the virtual range
  - Would require tracking allocated ranges (list or tree)
- [ ] No bounds check on mmio_next_virt
  - Could run off the end of MMIO region without warning
  - Add MMIO_END constant and check before advancing pointer

## Page-table location limit (measured)
- [x] TESTED: boots fine at 4 GB. Page tables (~28 KB) + bitmap (160 KB)
  fit well under physical 2 MB, so KP2V works.
- [ ] HARD LIMIT: ~32 GB RAM. At that point the PMM bitmap alone
  (32GB/4K/8 = ~1 MB) plus page tables pushes allocations past physical
  2 MB, where KP2V (kernel mapping = 0-2 MB only) faults.
  * 128 GB → 4 MB bitmap alone → guaranteed crash
- [ ] FIX (when needed): two-phase paging bootstrap
  * Phase 1: bootloader pre-maps first ~16 MB into kernel range
    OR build a minimal direct map covering where page tables will live
  * Phase 2: build full direct map using P2V (direct map access)
  * Then switch VMM table access from KP2V to P2V

## Architecture

- [ ] Single CPU only - SMP support not implemented
- [ ] No APIC setup - still using legacy PIC (when we add interrupts)
- [ ] No ACPI parsing yet

### Console / Display
- [ ] Build a proper console abstraction layer (console.h/c)
  - kprintf writes to console, not serial directly
  - Console routes to multiple backends (serial + screen)
- [ ] Get framebuffer access during boot
  - Option A: VBE/VESA via Stage 2 BIOS calls (BIOS)
  - Option B: UEFI GOP (modern, requires UEFI bootloader)
- [ ] Build text rendering on framebuffer
  - Bitmap font (8x16 standard PC font)
  - Glyph rendering routine
  - Scrolling, cursor, color
- [ ] Eventually: GPU acceleration
  - PCI enumeration to find GPU
  - GPU driver
  - 2D acceleration (rectangle blit, glyph cache)

## Phase 6 - Framebuffer

#### Stage 2 VBE - Proper Mode Selection (Future)
Currently hardcoded to mode 0x118 (1024x768x24bpp). Real implementation:

1. EDID Query (INT 10h AX=4F15h, BL=01h)
   - Returns 128-byte EDID block from monitor
   - Parse bytes 0x36-0x47 (detailed timing descriptor 1)
   - Extract native horizontal/vertical resolution
   - Fall back gracefully if EDID unavailable

2. VBE Mode Enumeration
   - Read VbeModeListPtr from VBEInfoBlock (offset 0x0E)
   - This is a far pointer (segment:offset) to uint16_t array
   - Array terminated by 0xFFFF
   - For each mode, call 4F01h and read ModeInfoBlock

3. Mode Filtering & Scoring
   - Require: ModeAttributes bit 0 (supported), bit 4 (graphics), bit 7 (LFB)
   - Require: MemoryModel == 6 (direct color)
   - Score: closeness to EDID native resolution
   - Tiebreaker: prefer 32bpp > 24bpp > 16bpp

4. Fallback Chain (if EDID/scoring fails)
   - Try 1920x1080, 1280x1024, 1024x768, 800x600, 640x480
   - For each, search mode list for a match
   - Final fallback: stay in text mode with VGA at 0xB8000

### vmm_map_mmio
- [x] Now uses VMM_NOCACHE (PCD bit) - correct for register MMIO
- [ ] Add vmm_map_mmio_wc() for write-combining (framebuffer, large buffers)
  - Requires PAT (Page Attribute Table) setup in IA32_PAT MSR (0x277)
  - Then PWT bit + PAT bit in page table entry select WC slot
  - See Intel SDM Vol 3, section 11.12
- [ ] No deallocation — bump allocator only
- [ ] No bounds check on mmio_next_virt — could overflow MMIO_BASE region


## Phase 7 — Hardware Interrupts (deferred items)

### PIC / Interrupt Controller
- [ ] Spurious IRQ 7 / IRQ 15 detection
  * When PIC receives a spurious interrupt (line noise, race), it sends
    IRQ 7 (master) or IRQ 15 (slave) with ISR bit unset
  * Should read PIC ISR register and check bit before sending EOI
  * Without this, spurious IRQs cause the PIC to lock up
- [ ] Migration to local APIC + IO APIC
  * Required for SMP support
  * Discovered via ACPI MADT (Multiple APIC Description Table)
  * Local APIC is memory-mapped (typically 0xFEE00000)
  * IO APIC handles external IRQs, replaces master/slave PIC
  * Need to mask all PIC IRQs and disable PIC before enabling APIC
- [ ] Save IRQ mask state in irq_register, restore in irq_unregister
- [ ] irq_handler should pass register state to handler (currently no args)

### PIT / Timer
- [ ] Reprogram PIT to 1000 Hz (1ms resolution)
  * Currently using BIOS default rate
  * Write divisor to channel 0 via ports 0x40 and 0x43
  * Divisor = 1193182 / target_hz
- [ ] Migrate to TSC + HPET for precise time
  * HPET (High Precision Event Timer) for one-shot timers
  * TSC (Time Stamp Counter) for high-resolution time measurements
  * Both discovered via ACPI
- [ ] Tick handler currently does only counter++
  * No process scheduling yet (no processes either)
  * Eventually: preemption check, scheduler invocation

### Keyboard
- [ ] Only ASCII press events handled
  * Add release tracking for proper modifier state
- [ ] No modifier handling (Shift, Caps Lock, Ctrl, Alt)
  * Need separate state machine tracking modifier press/release
  * Shift+letter → uppercase, shift+number → symbol
  * Caps Lock toggles, doesn't latch like Shift
- [ ] No extended scan codes (0xE0 prefix)
  * Arrow keys, Home/End/PgUp/PgDn, Insert/Delete, right-side Ctrl/Alt
  * Multi-byte scan code state machine needed
- [ ] No F1-F12 keys (scan codes 0x3B-0x44)
- [ ] No Windows/Menu keys (0xE0 prefix)
- [ ] No layout abstraction
  * Only US QWERTY hardcoded
  * Should support keymap loading (AZERTY, Dvorak, etc.)
- [ ] No PS/2 controller initialization
  * Currently relies on BIOS leaving controller in usable state
  * Should explicitly configure 8042 via port 0x64 commands
  * Disable mouse port, set scan code set, enable IRQs
- [ ] Migration to USB HID when we have USB stack

## Phase 8 — Heap (deferred)
- [ ] No locking in kmalloc/kfree — corruption if called from IRQ while
  main context is mid-allocation. Need spinlock with cli/sti save-restore.
- [ ] No slab allocator (Phase 8b) — add fixed-size pools (16/32/64/...)
  on top of linked-list for O(1) common-case alloc
- [ ] krealloc could expand in-place if next block is free (currently always
  copies)
- [ ] No guard pages between allocations (security hardening)
- [ ] No allocation randomization (security hardening)
- [ ] No quarantine/delayed-free for use-after-free detection
- [ ] First-fit is O(n) — consider segregated free lists or best-fit
- [x] kmalloc/kfree locking — DONE: spinlock with cli/sti save-restore
- [ ] Spinlock has no deadlock detection / lock ordering checks
- [ ] No reader-writer locks (all mutual exclusion is exclusive)
- [ ] Per-CPU heap caches to reduce lock contention (multi-core, future)

## Phase 9b — APIC (deferred)
- [x] IO-APIC version register — resolved, reads 0x170020 = 24 entries correctly
- [ ] Interrupt source overrides from MADT not applied to IO-APIC routing
  (e.g. timer IRQ0->GSI2). Currently only keyboard (GSI1, no override).
  When routing more IRQs, must consult overrides + polarity/trigger flags.
- [ ] LAPIC timer not using TSC-deadline mode (more precise, modern)
- [ ] No per-CPU LAPIC init (needed when APs come online in SMP)
- [ ] Spurious-interrupt vector (0xFF) installed in SVR but no IDT handler
  for it — should add a no-op handler to be safe
- [ ] Duplicated register save/restore between irq_common and
  irq_commom_lapic in isr.asm — refactor to share


## Phase 10 — PCI (deferred)
- [ ] No BAR parsing — need to read Base Address Registers to find each
  device's MMIO/IO regions (size via write-all-1s-then-read trick)
- [ ] No ECAM/MCFG (PCIe memory-mapped config) — only legacy CAM
  Parse MCFG ACPI table, map config space, support 4KB extended config
- [ ] No recursive bridge scanning — brute-force works but doesn't follow
  secondary buses behind PCI-to-PCI bridges properly
- [ ] No capability list parsing (MSI/MSI-X, power management)
- [ ] No interrupt routing — need to wire device INTx/MSI to IO-APIC/LAPIC
- [ ] No device-specific driver binding mechanism yet
- [ ] Vendor/device ID -> human name database (currently only class names)

- [x] BAR parsing — DONE (Phase 10b): I/O + 32/64-bit MMIO, sizing
- [ ] BARs not yet stored in pci_device struct — only printed.
  Should cache them so drivers can retrieve without re-reading.
- [ ] No ECAM/MCFG, no recursive bridges, no capabilities (still pending)


## Phase 11c — ATA DMA (deferred / next)
- [ ] Multi-PRD scatter-gather for buffers > 64KB or spanning pages
  (current limit: single contiguous region <= 64KB, <= 128 sectors)
- [ ] LBA48 DMA (READ DMA EXT 0x25) for >128 sectors / disks > 128GB
- [ ] DMA buffer currently must be kernel BSS/contiguous (KV2P).
  For arbitrary virtual buffers: walk pages, build multi-PRD, or bounce-buffer
- [ ] No cache coherency handling (fine on QEMU/x86; matters on some HW)
- [ ] Secondary channel DMA (offset 0x08 in BMIDE) not handled
References:
- VBE 3.0 spec (Function 15h: Display Data Channel)
- EDID 1.4 spec (VESA E-EDID)