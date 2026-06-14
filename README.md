# Helios

A 64-bit x86_64 operating system built entirely from scratch — no GRUB, no pre-built bootloader, no shortcuts. The goal isn't a toy that boots in QEMU; it's a real system where every layer, from the boot sector to the page tables, is written and understood by hand.

## What it is

Helios starts from a 512-byte boot sector and works its way up: real mode → protected mode → long mode → a higher-half kernel running at `0xFFFFFFFF80100000`. Along the way it parses the BIOS memory map, sets up paging, manages physical and virtual memory, handles CPU exceptions, and brings up modern interrupt hardware.

It is written in C and x86_64 assembly (NASM), built with an `x86_64-elf` cross-compiler, and tested under QEMU with GDB for source-level debugging.

## Features

- **Custom two-stage BIOS bootloader** — stage 1 (boot sector) loads stage 2, which queries the E820 memory map, enables the A20 line, switches through protected mode into long mode, and parses the kernel ELF to load its segments.
- **Higher-half kernel** at `0xFFFFFFFF80100000`, mapped via a linker script with physical/virtual split.
- **Interrupt handling** — full IDT with ISR stubs for all 32 CPU exceptions and a C-level fault handler that dumps register state over serial.
- **Physical memory manager** — bitmap allocator driven by the E820 map.
- **Virtual memory manager** — 4-level paging, map/unmap/translate, higher-half direct map of physical RAM.
- **ACPI parsing** — locates and walks the ACPI tables (MADT) to enumerate CPUs and interrupt controllers.
- **APIC interrupts** — retired the legacy 8259 PIC in favour of the Local APIC / IO-APIC.
- **Serial + `kprintf`** — formatted kernel logging over COM1 for development and debugging.

### In progress

- **AHCI driver** — SATA disk access (controller located, ABAR mapped, port detection working; full read/write implementation underway).

### Roadmap

- VBE framebuffer + console abstraction (route `kprintf` to screen as well as serial)
- Buddy/free-list physical allocator to replace the linear bitmap scan
- SMP (multi-core) bring-up — the CPU list is already available from the MADT
- UEFI boot path alongside BIOS
- ARM64 port

## Building and running

You'll need an `x86_64-elf` cross-compiler, NASM, QEMU, and GNU Make.

```bash
# Build the disk image
make

# Boot it in QEMU (serial output goes to your terminal)
make run

# Boot paused for GDB on :1234
make debug
```

In a second terminal, attach the debugger:

```bash
gdb        # .gdbinit auto-connects to QEMU and breaks at kernel_main
```

## Project layout

```
boot/stage1/    Boot sector (512 bytes)
boot/stage2/    Stage 2: E820, A20, long mode, ELF loader
kernel/cpu/     IDT, ISR stubs, exception handlers
kernel/mm/      Physical (PMM) and virtual (VMM) memory managers
kernel/drivers/ Serial and hardware drivers
kernel/         Kernel entry, kprintf, linker script
docs/           GDB cheat sheet and notes
```

## License

Released under the MIT License. See [LICENSE](LICENSE).
