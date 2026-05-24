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

**Phase 10 — Candidates (pick one):**
1. SMP bringup — wake the other 3 CPUs (we have their APIC IDs).
   Send INIT-SIPI-SIPI via LAPIC, give each core a stack, bring online.
   Big, exciting, the natural payoff of all the APIC work.

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

