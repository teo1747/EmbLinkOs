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
- Currently maps first 1GB of physical RAM (TODO: expand)

### Phase 5 — kprintf ✅
- Full format specifier support: %d %u %x %X %p %s %c %% %lx
- Width and zero-padding modifiers
- Uses GCC __builtin va_list

## Current State
- Boots cleanly in QEMU (`make run`)
- Kernel runs at 0xFFFFFFFF80100000
- All exceptions catchable via IDT
- Memory management functional
- Debug output via COM1 serial

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
│   ├── drivers/
│   │   └── serial.h, serial.c
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
**Phase 6 — Framebuffer + Console Abstraction**

Plan:
1. VBE mode setup in Stage 2 (INT 10h 4F00h, 4F01h, 4F02h)
2. Pass framebuffer physical address to kernel
3. Framebuffer driver with embedded 8x16 font
4. Console abstraction layer (kprintf → serial + framebuffer)
5. Later: swap embedded font for PSF file

Just starting. Need to read https://wiki.osdev.org/VESA_Video_Modes

## Teaching Style Preferences
- Go slow, line by line
- Explain every SDM section involved
- Ask questions to verify understanding before moving on
- Don't skip the "why" — every design decision matters
- User answers questions, then writes code, then we debug together
- User is electronics/embedded background, junior engineer level

## Known Limitations (See TODO.md For Full List)
- VMM only maps first 1GB physical (breaks on real hardware >1GB)
- Stack hardcoded at 0x200000
- No NX bit support yet
- Single core only
- BIOS-only (no UEFI)
- Hardcoded 90 sectors loaded for kernel