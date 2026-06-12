# Helios OS - Project Status & Handoff

## Project Overview
Building a complete x86_64 operating system from absolute zero. No GRUB, no shortcuts, every line understood. Will eventually support ARM64 and SMP.

## Design Philosophy
- Slow and deliberate, not fast
- Understand every register, every bit, every SDM section
- Document known issues in TODO.md as we go
- SMP-aware design from day one (even if single-core for now)

## Naming
- Project: Helios
- Architecture: x86_64 first, ARM64 later
- Cores: Single-core, designed for SMP
- Kernel virtual base: 0xFFFFFFFF80000000 (higher half)

## Completed Phases

### Phase 1 — Bootloader ✅
- Custom Stage 1 (512 bytes, real mode, MBR signature, INT 13h)
- Custom Stage 2 (4KB, switches Real → Protected → Long Mode)
- LBA disk loading via INT 13h ext 0x42 (per-sector loop to avoid BIOS limits)
- E820 memory map query
- A20 enable
- GDT (32-bit + 64-bit)
- Dual page table mapping (identity + higher half)
- ELF kernel loader

### Phase 2 — IDT & Exceptions ✅
- 256-entry IDT
- ISR stubs for exceptions 0-31 in NASM
- C handler dumps register state
- Catches divide-by-zero, page fault, etc.

### Phase 3 — Physical Memory Manager ✅
- Bitmap allocator
- Dynamic sizing from E820 highest address
- Bitmap placed at kernel_end (linker symbol)
- V2P/P2V macros for higher-half translation
- pmm_alloc_page() returns physical addresses
- Properly reserves low memory, kernel, bitmap, stack

### Phase 4 — Higher Half Kernel + VMM ✅
- Linker script links at 0xFFFFFFFF80100000
- Stage 2 maps both identity AND higher half
- Far jump to higher half before kernel runs
- Custom 4KB-page VMM replaces bootloader 2MB tables
- vmm_map, vmm_unmap, vmm_get_phys, vmm_flush_tlb working

### Phase 4.1a — Full Direct Map ✅
- New virtual memory layout (Linux-style)
- Direct map covers all usable physical RAM (from E820)
- Uses 2 MB huge pages for direct map (efficient)
- Kernel code stays at 0xFFFFFFFF80000000
- Separate KV2P/KP2V macros for kernel-range conversion
- Successfully tested on 128 MB QEMU

### Phase 4.1b — vmm_map_mmio() helper ✅
- Add ability to dynamically map MMIO regions (framebuffer, PCI BARs)
at MMIO_BASE virtual range using 4 KB pages.


### Phase 5 — kprintf ✅
- Full format specifier support: %d %u %x %X %p %s %c %% %lx
- Width and zero-padding modifiers
- Uses GCC __builtin va_list


### Phase 6.1 — Framebuffer Driver ✅
- VBE mode 0x118 setup in Stage 2 (1024x768x24bpp, BGR, LFB)
- VBE info struct passed to kernel via physical 0x6000
- Framebuffer dynamically mapped via vmm_map_mmio
- fb_put_pixel handles RGB and BGR formats
- fb_clear fills screen with solid color


### Phase 6.2 — Bitmap Font Rendering
1. Embed an 8x16 PC font (binary array, ~4KB)
2. Render single glyphs at (x, y) with fg/bg colors
3. Render strings with line wrapping and scrolling
4. Maintain a cursor (col, row)
5. Later (Phase 6.3): console abstraction + kprintf routing


### Phase 6.3 — Framebuffer + Console ✅
- Framebuffer driver: fb_put_pixel, fb_clear, fb_draw_char, fb_draw_string
- Embedded IBM VGA 8x16 font (public domain), ASCII 0x00-0x7F
- Console abstraction: cursor, colors, scrolling, dual-backend (serial + FB)
- kprintf routes through console — appears on both serial and screen


### Phase 7a — Hardware Interrupts (PIC + Timer + Keyboard) ✅
- New kernel GDT (replaces bootloader GDT which lived at unmapped phys 0x7ebe)
  * 3 entries: null, kernel code (L=1), kernel data
  * Far return to reload CS, mov-based reload for data segments
- 8259 PIC driver
  * Remapped vectors: master 0x20-0x27, slave 0x28-0x2F
  * pic_send_eoi handles slave-then-master EOI ordering
  * pic_mask/unmask manipulate IMR per IRQ
- IRQ dispatcher
  * 16 IRQ stubs in isr.asm (irq0-irq15)
  * irq_install registers stubs into IDT 32-47
  * irq_register installs handler + auto-unmasks at PIC
- PIT timer (IRQ 0)
  * Default BIOS rate (~100 Hz on QEMU)
  * Minimal handler: increments volatile tick counter
- PS/2 keyboard (IRQ 1)
  * Set 1 scan codes from port 0x60
  * US QWERTY ASCII translation table
  * Circular buffer between IRQ and main thread
  * keyboard_getchar blocks via hlt


### Phase 8a — Kernel Heap Allocator ✅
- Linked-list first-fit allocator (kmalloc/kfree/kcalloc/krealloc)
- 16-byte aligned, block splitting + bidirectional coalescing
- Auto-grows from PMM, heap at 0xFFFFFF8000000000
- Heap canaries (head + tail) for corruption/double-free detection
- kheap_check + kheap_stats debug tools
- Verified with 100-alloc stress test, full coalesce on free

### Phase 8b-prep — Spinlocks + IRQ-safe heap ✅
- spinlock_t with atomic test-and-set + IRQ save/restore
- kmalloc/kfree now safe to call from interrupt context
- Verified under timer-IRQ vs main-loop heap contention (1.5M ops)
- Added kernel/include/types.h (NULL, bool, size_t)


### Phase 9b — APIC (replaces 8259 PIC) ✅
- Local APIC: MMIO mapped, enabled via MSR + spurious vector
- LAPIC timer: PIT-calibrated, periodic 100 Hz, vector 48, LAPIC EOI
- IO-APIC: redirection table programming
- Keyboard routed GSI 1 -> vector 33 -> CPU 0 via IO-APIC
- 8259 PIC fully masked and retired
- Interrupt path: device -> IO-APIC -> LAPIC -> CPU -> LAPIC EOI
- All prerequisites for SMP now in place

### Phase 10a — PCI Enumeration ✅
- Legacy port-based config access (0xCF8/0xCFC)
- Full brute-force bus/device/function scan
- Per-device: vendor, device, class, subclass, prog_if, header type
- Discovered device table for driver use
- io.h gained outl/inl
- Found on QEMU: 440FX bridge, PIIX3, IDE, ACPI, Bochs VGA, e1000

### Phase 10b — PCI BAR Parsing ✅
- Read/size all 6 BARs per device (I/O, 32-bit MMIO, 64-bit MMIO)
- Size detection via write-all-1s trick with restore
- Prefetchable detection
- Confirmed Bochs VGA BAR0 = framebuffer 0xFD000000
- e1000 NIC and IDE controller register regions located

### Phase 11a — ATA PIO Disk Driver (polled) ✅
- Multi-drive detection: scans primary + secondary, master + slave (4 candidates)
- IDENTIFY-based detection, LBA28 addressing (CHS fallback for tiny disks)
- ata_read_sectors / ata_write_sectors (PIO, 256 words/sector, polled)
- Per-drive struct: io_base, ctrl_base, master/slave, total_sectors, model
- io.h gained inw/outw (16-bit port I/O)
- Verified: boot disk (drive 0) sector 0 sig 0x55AA correct;
  64 MB data disk (drive 1) write/read test PASS — persistent storage works
- Makefile: DRIVES variable, separate disk.img data disk (preserved on clean)
- Fixed: stage2 load-size bug (kernel outgrew 90 sectors -> 512, with guard)

### Phase 11b — Interrupt-driven ATA ✅
- Reads and writes both IRQ-driven (IRQ 14 -> vector 46 via IO-APIC)
- Read: IRQ before transfer (data ready); Write: poll DRQ to accept,
  IRQ after transfer (sector committed); CACHE FLUSH IRQ-confirmed
- Handler acks device via status read, sets flag; wait sleeps on hlt
- Verified IRQ count matches op count (4 ops = 4 IRQs)

### Phase 11c — ATA DMA (bus mastering, read) ✅
- Bus mastering enabled in IDE PCI command register
- BMIDE registers at BAR4 (0xc040)
- Single-entry PRDT in BSS (KV2P for physical addr), EOT, <=64KB
- ata_read_dma: full bus-master sequence, completion via IRQ 14
- Zero CPU copy — controller writes RAM directly
- Verified sector 0 read via DMA, boot signature + bytes correct

### Phase 11c — ATA DMA (bus mastering) ✅
- Read (0xC8) and write (0xCA) both via bus-master DMA
- Direction bit: set for disk read (device writes RAM),
  cleared for disk write (device reads RAM)
- Single-entry PRDT in BSS (KV2P physical), EOT, <=64KB
- Completion via IRQ 14; cache flush after writes
- Zero-copy: controller moves all data; verified byte-exact round-trip

### Phase 11d-2 — AHCI command machinery + IDENTIFY ✅
- Full command path working: header -> table -> CFIS -> PRD -> PxCI
- Controller DMAs IDENTIFY data into kernel buffer
- 64MB SATA disk on port 0 fully identified

### Phase 11d-2 — AHCI command machinery + IDENTIFY ✅
- Full command path working: header -> table -> CFIS -> PRD -> PxCI
- Controller DMAs IDENTIFY data into kernel buffer
- 64MB SATA disk on port 0 fully identified

## Current State
- Boots cleanly in QEMU (`make run`)
- Kernel runs at 0xFFFFFFFF80100000
- All exceptions catchable via IDT
- Memory management functional
- Debug output via COM1 serial

## Next Steps


## File Structure
myos/
├── boot/
│   ├── stage1/boot.asm
│   └── stage2/stage2.asm
├── kernel/
│   ├── main.c
│   ├── linker.ld
│   ├── kprintf.c
│   ├── include/
│   │   ├── io.h
│   │   └── kprintf.h
│   ├── cpu/
│   │   ├── idt.h, idt.c
│   │   ├── isr.asm, isr.c
│   │   ├── gdt.h, gdt.c
│   ├── drivers/
│   │   └── serial.h, serial.c
|   |   └── framebuffer.h, framebuffer.c
|   |   └── keyboard.h, keyboard.c
│   └── mm/
│       ├── pmm.h, pmm.c
│       └── vmm.h, vmm.c
├── docs/
│   ├── GDB_CHEATSHEET.md
│   └── PROJECT_STATUS.md (this file)
├── Makefile
├── TODO.md
└── myos.img (build output, 1MB)

## Build Environment
- OS: Ubuntu Linux
- Toolchain: x86_64-elf cross compiler at /usr/local/cross/bin
- Assembler: NASM
- Emulator: QEMU
- Editor: VS Code
- Repository: github.com/teo1747/myos (rename pending to "helios")

## Build/Run Commands
```bash
make            # build everything
make run        # build and run in QEMU with serial→stdio
make debug      # run with GDB server on port 1234
make clean      # remove binaries
```

## Memory Layout
0x000000 - 0x000FFF  →  IVT, BIOS data
0x007000 - 0x007FFF  →  E820 memory map
0x007C00             →  Stage 1
0x007E00             →  Stage 2
0x009000             →  PML4 (bootloader)
0x00A000             →  PDPT identity (bootloader)
0x00B000             →  PD shared (bootloader)
0x00C000             →  PDPT higher (bootloader)
0x010000             →  Kernel ELF (raw, before parsing)
0x100000             →  Kernel (physical)
0x106000             →  PMM bitmap
0x108000+            →  Free pages for allocation
0x1F0000 - 0x200000  →  Kernel stack region

## Next Phase In Progress

###


3. Phase 8b — slab allocator (deferred performance work).


4. Filesystem (FAT12/16 on the disk image) — needed for loading files




Later: swap embedded font for PSF file


## Teaching Style Preferences
- Go slow, line by line
- Explain every SDM section involved
- Ask questions to verify understanding before moving on
- Don't skip the "why" — every design decision matters
- User answers questions, then writes code, then we debug together
- User is electronics/embedded background, junior engineer level

## Known Limitations (See TODO.md For Full List)
- Stack hardcoded at 0x200000
- No NX bit support yet
- Single core only
- BIOS-only (no UEFI)
- Hardcoded 90 sectors loaded for kernel

