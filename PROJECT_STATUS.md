# EmbLink OS — Project Status

Last updated: 2026-07-01

## Overview

EmbLink OS is a minimal 64-bit x86 kernel with:
- **Bootloader**: BIOS-based 2-stage loader with hardcoded kernel layout
- **Memory**: PMM + VMM with dynamic paging (up to 4GB sustained, ~32GB architectural limit)
- **Filesystems**: EMBKFS (native, transactional COW), FAT32 (R/W), VFS multiplexer
- **Drivers**: Serial console, ATA/DMA, AHCI, keyboard, VBE framebuffer, ACPI/APIC
- **Interrupts**: IDT + handler dispatch, exceptions, LAPIC timer, keyboard IRQ
- **User Mode**: Ring-3 entry via iretq, ELF64 loader, int 0x80 syscalls (write, exit)

---

## Completed Phases

### Phase 1: Bootloader + Kernel Entry (✅)
- 2-stage BIOS loader (stage1.bin loads stage2.bin, stage2 loads kernel)
- Paging enabled, GDT set, IDT stub, basic console I/O
- **Limitation**: hardcoded sector counts; kernel > 512 sectors requires manual padding

### Phase 2: Memory Management (✅)
- **PMM**: page allocator with O(n) bitmap scan
- **VMM**: 2-level page tables (no PAE), dynamic mapping, NX support
- **MMIO**: bump allocator (no dealloc yet)
- **Heap**: 
  - Doubly-linked list of blocks with coalescing
  - **✅ krealloc in-place expansion**: avoids copies when next block is free and large enough
    - Checks adjacent block before allocating new
    - Coalesces on-the-fly if space available
    - Reduces fragmentation for dynamic array growth (EMBKFS use case)
  - Canary-based corruption detection
  - Spinlock-protected for concurrent access
  - Slab allocator: DEFERRED (prototype had metadata collision issues; needs separate tracking layer)
- **Limitation**: supports up to ~32 GB; page tables must fit in first 2 MB

### Phase 3: Filesystem Support (✅)
- **EMBKFS**: native transactional FS with B-tree, copy-on-write, atomic commits
  - Features: file/dir creation, unlink, open-ref tracking, in-memory orphan handling
  - **Limitation**: no crash-safe orphan reclaim; no symlinks yet
- **FAT32**: full R/W with long filenames, partition support
- **VFS**: multiplexed FS layer with path resolution, single-mount (multi-mount TODO)
- **FD Layer**: 64-fd global table, per-FD cursors, O_APPEND + O_TRUNC (no dup/fork yet)

### Phase 4: Drivers (✅)
- **Serial**: 16550A console (output only, polling)
- **ATA**: DMA+IRQ driven, LBA28, multi-PRD scatter-gather, bounce-buffer fallback
- **AHCI**: SATA support, port 0 tested, polling (IRQ-driven TODO)
- **Block**: partition parsing (MBR), child devices per partition
- **Keyboard**: PS/2 IRQ9, ASCII only (modifiers/extended-codes TODO)
- **VBE**: 1024×768×24-bit framebuffer (mode 0x118 hardcoded; mode selection TODO)
- **ACPI/APIC**: MADT parse, interrupt routing, LAPIC timer TSC-calibrated

### Phase 5: Interrupts + Timers (✅)
- **IDT**: 256 vectors, exception handlers with CR2/error-code decode
- **LAPIC timer**: 100 Hz tick (TSC-based one-shot, no preemption yet)
- **APIC**: IO-APIC routing for keyboard (GSI1), keyboard IRQ9 active
- **TSC + HPET**: TSC calibrated against HPET via ACPI; HPET MMIO for precise delays
- **Tick counter**: timer_get_ticks() tracks IRQ0 / LAPIC interrupts
- **Limitation**: single MADT override (timer GSI); MADT overrides not applied to all IRQs

### Phase 6: User Mode + Syscalls (✅ JUST LANDED)
- **ELF64 Loader**: parses PT_LOAD, maps segments with correct p_flags (NX, W/X)
- **Ring-3 Entry**: iretq to user code/data, user RSP, EFLAGS=0x202, interrupt-live
- **Context Save/Restore**: kernel_ctx_save captures RBP/RSP, allows resumption after user exit
- **int 0x80 Dispatch**: syscall table with SYS_write (fd=1, serial out) + SYS_exit (return to kernel)
- **Embedded ELF**: init_blob is now a proper ELF binary (replaces flat-binary stub copy)

---

## Major To-Do Buckets (Rough Priority)

### High Priority (block real programs)
1. **User-pointer validation** (`copy_from_user`): syscall args are unchecked
   - Unblock: per-process address spaces
2. **File I/O syscalls**: open/read/write/close/lseek/stat/readdir wired to VFS
   - Unblock: (1) above
3. **Processes**: create/exec/exit with per-process address space, fd table, stack
   - Unblock: (2) + scheduler

### Medium Priority (real-world use)
1. **Filesystems**:
   - EMBKFS: crash-safe orphan reclaim, symlink support (.readlink op)
   - VFS: multi-mount (longest-prefix match), real `.truncate` op
   - FAT32: already works; IDE secondary channel (DMA + IRQ wiring)
2. **Drivers**:
   - VBE: mode enumeration + EDID query (don't hardcode 1024×768)
   - Keyboard: modifiers (Shift/Ctrl/Alt), extended scancodes, F1–F12
   - Framebuffer: write-combining (vmm_map_mmio_wc), GPU accel later
   - ATA: LBA48, secondary channel, multi-disk
3. **Synchronization**: per-CPU heap caches (SMP prep)

### Lower Priority (architecture)
1. **SMP**: AP bring-up (INIT-SIPI-SIPI), per-CPU data, per-CPU LAPIC
2. **Syscall fast-path**: STAR/LSTAR/SFMASK for `syscall`/`sysret`
3. **Bootloader v2**: ELF-aware loading, UEFI support, USB/CD boot
4. **Slab allocator**: fixed-size pools on top of heap (currently linked-list only)

---

## Recent Commits (Last 10)

```
c8e60bd finish implementing load ring3 user space
d7e77ff usermode: re-enable IRQs on exit-via-kcontext return path
5974e65 cpu: kernel survives a user program exiting (kcontext unwind)
9dd4bfd user: load + run a real flat-binary program from an embedded blob
120f406 syscall: int 0x80 entry + table dispatch (write, exit)
0649dfb cpu: reach ring 3; exception dump shows CS/SS
f09cf1e gdt: add ring-3 user descriptors + 64-bit TSS
f2af17f d: open/close/read/write/seek + create-on-open, unlink-while-open proven
44a19eb embkfs: unlink-while-open safety (in-memory open refcount)
2b1fe16 vfs: ls consumer over the public VFS surface
```

---

## Known Limits & Caveats

### Memory
- **Bitmap-based PMM** is O(n); >32 GB RAM risks exceeding 2 MB kernel-accessible region
- **Page-table growth** capped at 2 MB; once kernel code/data hits 2 MB, unmapped regions fault
  - Workaround: currently kernel is ~1.5 MB; drivers + filesystem fit

### Filesystems
- **No crash-safe orphan reclaim** (EMBKFS): unlink-then-crash loses inodes
- **No symlinks** (EMBKFS): .readlink op missing from VFS
- **Single mount only**: multiplexer returns the one configured "/" regardless of path

### Drivers
- **No multi-IRQ routing** from MADT: only keyboard (GSI1, no override) routed
- **VBE mode hardcoded**: 1024×768×24; EDID + mode enumeration TODO
- **IDE secondary channel** not wired: ATA DMA is primary-only (IRQ14, BMIDE base)
- **Keyboard** ASCII-only: no modifiers, extended codes, or function keys

### User Mode
- **Unchecked user pointers**: ring-3 can pass kernel addresses to sys_write
  - Requires per-process address spaces to fix
- **Minimal syscall table**: only write + exit; no open/read/close yet

---

## Test Status

| Subsystem | Status | Notes |
|-----------|--------|-------|
| PMM | ✅ Passes | Allocates/frees pages consistently |
| VMM | ✅ Passes | Maps kernel + user, NX works |
| EMBKFS | ✅ Passes | Create/read/write/unlink/mkdir verified; orphan leak on crash known |
| FAT32 | ✅ Passes | Read/write long filenames on MBR partition |
| VFS | ✅ Passes | Path resolution, multi-FS discovery, basic ls |
| ATA | ✅ Passes | DMA+IRQ on primary channel, MBR partition discovery |
| AHCI | ✅ Passes | Read/write on port 0 (polling) |
| Ring-3 Entry | ✅ Passes | ELF loads, user code runs, iretq works, syscalls dispatch |
| Context Switch | ✅ Passes | User exit restores kernel context |
| LAPIC Timer | ✅ Passes | 100 Hz ticks, TSC calibrated |
| rwlock | ✅ Passes | Reader/writer locking, smoke test passes |

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
   │  write(fd, buf, len), exit(code)     │
   └──────────────┬───────────────────────┘
                  │
       ┌──────────┼──────────┐
       ↓          ↓          ↓
    ┌────┐    ┌────┐    ┌────────┐
    │VFS │    │  FD  │  │ Kernel │
    │    ├────┤Layer├────┤Context │
    └────┘    └────┘    └────────┘
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
    └─→ Partitions (MBR)

   Interrupts / Timers
    │
    ├─→ LAPIC (timer, local)
    ├─→ IO-APIC (keyboard GSI1)
    └─→ IDT (256 vectors, exceptions + IRQs)

   Memory
    │
    ├─→ PMM (page allocator, bitmap)
    ├─→ VMM (2-level paging, NX)
    └─→ Heap (kmalloc/kfree, spinlock)
```

---

## Next Steps (as of 2026-07-01)

1. **Validate user-pointer syscalls**: build a test program that passes kernel
   pointers to sys_write; confirm it reads kernel memory (document the vuln).
2. **Add file I/O syscalls**: open/read/close for user programs; wires file I/O
   to the existing VFS.
3. **Per-process address spaces**: separate user VA ranges, fd tables, allow
   multiple user programs (fork/exec framework later).
4. **Syscall argument validation**: copy_from_user / access_ok for syscall
   buffers; blocks untrusted user code.

---

## Useful References

- **Bootloader**: boot/stage1.bin + boot/stage2/stage2.asm
- **Kernel Entry**: kernel/cpu/main.c / kernel/cpu/init.asm
- **ELF Loader**: kernel/cpu/elf.c
- **User Mode**: kernel/cpu/usermode.c + kernel/cpu/gdt.c
- **Syscalls**: kernel/cpu/syscall.c + kernel/cpu/isr.asm
- **VFS**: kernel/fs/vfs.c / kernel/fs/fd.c
- **EMBKFS**: kernel/fs/embkfs/embkfs.c
- **FAT32**: kernel/fs/fat32.c
- **ATA/AHCI**: kernel/drivers/storage/ata.c + ahci.c
- **APIC**: kernel/drivers/apic.c
- **Memory**: kernel/mm/pmm.c + kernel/mm/vmm.c
