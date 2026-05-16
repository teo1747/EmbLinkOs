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

References:
- VBE 3.0 spec (Function 15h: Display Data Channel)
- EDID 1.4 spec (VESA E-EDID)