# Helios - Known Issues & Improvements

## Bootloader

### Stage 1
- [ ] Currently uses CHS for loading Stage 2 - should switch to LBA for consistency
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
- [ ] No support for memory >128MB testing yet (need to verify on larger RAM)
- [ ] Linear scan for free page is O(n) - slow with lots of allocations
  - Future: free list or buddy allocator

## Architecture

- [ ] Single CPU only - SMP support not implemented
- [ ] No APIC setup - still using legacy PIC (when we add interrupts)
- [ ] No ACPI parsing yet
