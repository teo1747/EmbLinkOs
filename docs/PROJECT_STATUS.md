# EmbLink OS — Project Status & Handoff

## Project Overview
Building a complete x86_64 operating system from absolute zero. No GRUB, no
shortcuts, every line understood. Long-term goal: run real software (Level 4),
with the OS eventually self-hosting and supporting ARM64 + SMP.

## Design Philosophy
- Slow and deliberate, not fast — understand every register, bit, and SDM section.
- Build bottom-up; defer nothing that the next layer truly depends on.
- Track open issues in TODO.md; record completed work here.
- SMP-aware and portability-minded from the start (arch-specific code kept
  identifiable so an ARM64 port later is a contained campaign, not a rewrite).

## Naming & Family
- Project: **EmbLink** (Embedded + Link).
- **EmbLink OS** — this project: general-purpose x86_64 → ARM64 OS.
- **EmbLink-RTOS** — separate sibling project: real-time OS for Cortex-M
  (own repo, own kernel; shares brand, philosophy, and copied leaf utilities,
  NOT kernel core). Future goal: the two communicate over a message protocol
  (UART now, AMP later), with EmbLink OS providing tools for the RTOS.
- Code prefix: `embk_` for kernel-internal subsystems.
- Kernel virtual base: 0xFFFFFFFF80000000 (higher half).

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

### Phase 6 — Framebuffer + Console ✅
- VBE mode 0x118 (1024×768×24bpp, LFB) set in Stage 2, info passed to kernel.
- Framebuffer mapped via vmm_map_mmio; fb_put_pixel / fb_clear / fb_draw_char.
- Embedded IBM VGA 8×16 font (public domain), ASCII 0x00–0x7F.
- Console abstraction: cursor, colors, scrolling, dual backend (serial + FB).
- kprintf routes through console → appears on both serial and screen.

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

### Supporting infrastructure ✅
- **errno**: EMBK_* error codes (POSIX-aligned values) + embk_strerror().
  Convention: int + error codes for fallible ops, bool for true/false questions.
- **kstring**: kernel mini-libc (memcpy, memset, memmove, memcmp, strlen,
  strcmp, strncmp, strcpy, strncpy, strcat, strchr) under standard names
  (GCC may emit implicit memcpy/memset).
- **snprintf + kprintf refactor**: both now wrap one format_string core driven
  by an output-sink callback (serial sink vs bounded-buffer sink). snprintf is
  bounds-safe and returns the would-be length (truncation detectable).

---

## Current State
- Boots cleanly in QEMU (`make run`, `make run-ahci`).
- Kernel at 0xFFFFFFFF80100000; ~270 sectors.
- Full memory management, interrupts, storage stack, block layer operational.
- Debug output via COM1 serial + framebuffer console.

## Next Steps (roadmap to Level 4 — run real software)
Dependency order:
1. **Filesystem** (FAT32 first — simplest, real-world compatible) on the block
   layer — turns sectors into files. ← NEXT
2. **VFS** layer (generic file operations, mountable filesystems).
3. **User mode (ring 3)** + TSS.
4. **System calls** (syscall/sysret ABI).
5. **Per-process address spaces** (VMM already has the primitives).
6. **ELF loader** → run a program from disk (= Level 1).
7. **Scheduler + processes** (fork/exec/wait, LAPIC-timer preemption).
8. **libc port** (newlib/musl — implement syscall backends, don't write from
   scratch) → hinge to running existing software.
9. **Shell** (= Level 2), then coreutils/busybox (Level 3→4), then self-hosting
   toolchain (tcc), then mouse + compositor + GUI.

---

## Build Environment
- OS: Ubuntu Linux. Toolchain: x86_64-elf cross compiler at /usr/local/cross/bin.
- NASM, QEMU, GNU Make, VS Code, GDB.
- Repository: github.com/teo1747/Helios (rename to EmbLink pending).

## Build / Run Commands
```bash
make            # build everything
make run        # boot in QEMU (serial → stdio)
make run-ahci   # boot with an extra AHCI SATA disk attached
make debug      # GDB server on :1234 (paused)
make clean      # remove binaries (preserves disk.img / ahci.img)
```

## Memory Layout (physical, low region)
```
0x000000 – 0x000FFF   IVT, BIOS data
0x007000 – 0x007FFF   E820 memory map
0x007C00              Stage 1
0x007E00              Stage 2
0x009000 – 0x00CFFF   Bootloader page tables (PML4/PDPT/PD)
0x010000              Kernel ELF (raw, before parsing)
0x100000              Kernel (physical load address)
kernel_end            PMM bitmap, then free pages
0x1F0000 – 0x200000   Kernel stack region
```
Virtual: kernel at 0xFFFFFFFF80000000; direct map at DIRECT_MAP_BASE
(0xFFFF800000000000); MMIO at MMIO_BASE (0xFFFFC00000000000); heap at
0xFFFFFF8000000000.

## Teaching Style Preferences
- Go slow, line by line; explain every SDM section involved.
- Ask questions to verify understanding before moving on; don't skip the "why".
- User answers questions, then writes the code, then we debug together.
- User is electronics/embedded background, junior engineer level.
- Deliver code directly, then explain. Tell the user when to commit + a message.

## Known Limitations (see TODO.md for the full, current list)
- VMM kernel mapping hardcoded to first 2 MB — will break when the kernel
  crosses 2 MB (page-fault). Fix: map up to rounded-up KV2P(kernel_end).
- Page tables constrained to first 2 MB physical (KP2V) → ~32 GB RAM hard limit.
- Stack hardcoded at 0x200000; no NX usage yet; single-core; BIOS-only (no UEFI).
- Bootloader loads a fixed 512 sectors (no ELF-aware sizing yet).