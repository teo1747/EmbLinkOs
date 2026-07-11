ASM       = nasm
ASM_FLAGS = -f bin
CC = x86_64-elf-gcc
# -Ikernel makes every kernel translation unit include project headers by
# their canonical path from the kernel/ root (e.g. #include
# "drivers/char/serial.h", "arch/x86_64/irq/idt.h"), independent of where the
# including file itself lives -- the standard approach and what keeps a move
# like this one from being fragile.
CFLAGS = -ffreestanding -nostdlib -nostartfiles \
         -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
         -mcmodel=kernel \
         -Ikernel \
         -g -O0

IMG = myos.img
DISK = disk.img
# All intermediate build artifacts (assembled .o, the AP trampoline .bin) land
# here, out of the source tree.
BUILD = build

# Userland rules appear before `all`, so pin the default goal explicitly.
.DEFAULT_GOAL := all

STAGE1_SRC  = boot/stage1/boot.asm
STAGE2_SRC  = boot/stage2/stage2.asm
ISR_ASM     = kernel/arch/x86_64/irq/isr.asm
KCONTEXT_ASM = kernel/arch/x86_64/cpu/kcontext.asm
KENTRY_ASM  = kernel/arch/x86_64/cpu/kentry.asm
KENTRY_OBJ  = $(BUILD)/kentry.o
ISR_OBJ     = $(BUILD)/isr.o
SYSCALL_ASM = kernel/arch/x86_64/syscall/syscall_entry.asm
SYSCALL_OBJ = $(BUILD)/syscall_entry.o
KCONTEXT_OBJ = $(BUILD)/kcontext.o

# --- SMP: AP entry stub (linked into the kernel image) + the real-mode
# trampoline (a separate flat binary, embedded via incbin -- see
# kernel/arch/x86_64/smp/ap_trampoline_blob.asm's comment for why this mirrors
# the tree's old init_blob.asm technique).
AP_ENTRY_ASM         = kernel/arch/x86_64/smp/ap_entry.asm
AP_ENTRY_OBJ         = $(BUILD)/ap_entry.o
AP_TRAMPOLINE_ASM    = kernel/arch/x86_64/smp/ap_trampoline.asm
AP_TRAMPOLINE_BIN    = $(BUILD)/ap_trampoline.bin
AP_TRAMPOLINE_BLOB_ASM = kernel/arch/x86_64/smp/ap_trampoline_blob.asm
AP_TRAMPOLINE_BLOB_OBJ = $(BUILD)/ap_trampoline_blob.o

KERNEL_SRC = kernel/main.c \
             kernel/selftests.c \
             kernel/arch/x86_64/irq/isr.c \
             kernel/arch/x86_64/irq/pic.c \
             kernel/arch/x86_64/irq/irq.c \
             kernel/arch/x86_64/irq/idt.c \
             kernel/arch/x86_64/irq/ioapic.c \
             kernel/arch/x86_64/irq/lapic.c \
             kernel/arch/x86_64/cpu/gdt.c \
             kernel/arch/x86_64/cpu/percpu.c \
             kernel/arch/x86_64/cpu/fpu.c \
             kernel/arch/x86_64/cpu/spinlock.c \
             kernel/arch/x86_64/cpu/rwlock.c \
             kernel/arch/x86_64/smp/smp.c \
             kernel/arch/x86_64/syscall/syscall.c \
             kernel/arch/x86_64/syscall/usercopy.c \
             kernel/arch/x86_64/syscall/usermode.c \
             kernel/arch/x86_64/syscall/elf.c \
             kernel/process/process.c \
             kernel/acpi/acpi.c \
             kernel/drivers/char/serial.c \
             kernel/drivers/video/framebuffer.c \
             kernel/drivers/video/gpu.c \
             kernel/drivers/video/bochs_vbe.c \
             kernel/drivers/video/virtio_gpu.c \
             kernel/drivers/video/font_8x16.c \
             kernel/drivers/video/console.c \
             kernel/drivers/video/bootanim.c \
             kernel/drivers/timer/timer.c \
             kernel/drivers/timer/hpet.c \
             kernel/drivers/timer/pit.c \
             kernel/drivers/timer/rtc.c \
             kernel/drivers/input/keyboard.c \
             kernel/drivers/bus/pci.c \
             kernel/drivers/usb/usb.c \
             kernel/drivers/usb/usb_core.c \
             kernel/drivers/usb/xhci.c \
             kernel/drivers/usb/ehci.c \
             kernel/drivers/usb/uhci.c \
             kernel/drivers/usb/ohci.c \
             kernel/drivers/storage/ata.c \
             kernel/drivers/storage/ahci.c \
             kernel/block/block.c \
             kernel/block/partition.c \
             kernel/mm/pmm.c \
             kernel/mm/vmm.c \
             kernel/mm/kheap.c \
             kernel/mm/kmalloc.c \
             kernel/crypto/sha256.c \
             kernel/crypto/hmac.c \
             kernel/crypto/pbkdf2.c \
             kernel/crypto/aes.c \
             kernel/crypto/xts.c \
             kernel/fs/fat32.c \
             kernel/fs/embkfs/embkfs.c \
             kernel/fs/embkfs/embkfs_compress.c \
             kernel/fs/embkfs/crc32c.c \
             kernel/fs/embkfs/embk_vfs.c \
             kernel/fs/fd.c \
             kernel/fs/vfs.c \
             kernel/lib/kstring.c \
             kernel/lib/errno.c \
             kernel/lib/kprintf.c

LINKER      = kernel/linker.ld
STAGE1_BIN  = boot/stage1/boot.bin
STAGE2_BIN  = boot/stage2/stage2.bin
KERNEL_ELF  = kernel/kernel.elf


# ---- Userland ---------------------------------------------------------------
# Layout: user/lib/ is the shared userland library -- the EmbLink SDK (embk.h,
# embk_syscall.h), the newlib retargeting layer (crt0.c, syscalls.c), and the
# linker scripts. user/bin/ holds the actual programs. Built artifacts (.o,
# .elf) go to $(BUILD), out of the source tree. -Iuser/lib lets a program in
# user/bin include the SDK as "embk.h". See user/README.md.
USER_CC      = x86_64-elf-gcc
USER_LD      = x86_64-elf-ld
USER_INC     = -Iuser/lib
# Freestanding programs (init.elf): own _start, no libc, no SSE.
USER_CFLAGS  = -ffreestanding -nostdlib -fno-pic -mno-red-zone \
               -fno-stack-protector -mno-mmx -mno-sse -mno-sse2 -O2 $(USER_INC)

build/init.o: user/bin/init.c | $(BUILD)
	$(USER_CC) $(USER_CFLAGS) -c $< -o $@

build/init.elf: build/init.o user/lib/user.ld
	$(USER_LD) -T user/lib/user.ld build/init.o -o $@

# --- newlib-linked userland (user/lib/crt0.c + user/lib/syscalls.c + a program) ---
# Unlike init.elf (freestanding, -mno-sse, own _start), these link against a
# newlib libc: normal SSE-using code (safe now that the kernel saves/restores
# FPU/SSE state across context switches), the crt0.c stack-alignment stub, and
# syscalls.c's POSIX retargeting stubs.
#
# NEWLIB_PREFIX: the toolchain's stock newlib (/usr/local/cross) was built
# WITHOUT C99 formats / long-long in printf (%z, %ll compiled out of libc.a).
# We rebuilt newlib with --enable-newlib-io-c99-formats + --enable-newlib-io-
# long-long into this user-owned prefix; pointing -L/-isystem here uses that
# fuller libc.a instead. Set NEWLIB_PREFIX= (empty) to fall back to the stock
# toolchain newlib (and re-lose %z/%ll). See user/README.md.
NEWLIB_PREFIX ?= /home/motsou/cross/newlib-c99
NEWLIB_INC    = $(if $(NEWLIB_PREFIX),-isystem $(NEWLIB_PREFIX)/x86_64-elf/include,)
NEWLIB_LIB    = $(if $(NEWLIB_PREFIX),-L$(NEWLIB_PREFIX)/x86_64-elf/lib,)
NEWLIB_CFLAGS = -mno-red-zone -fno-stack-protector -O2 -Wall $(USER_INC) $(NEWLIB_INC)
# gcc as the link driver so it finds libc.a/libgcc; -nostartfiles because
# crt0.c provides _start (no standard crtX). newlib.ld places it at 0x400000.
# NEWLIB_LIB is a -L searched BEFORE the toolchain's default lib dir, so our
# rebuilt libc.a wins over the stock one.
NEWLIB_LDFLAGS = -nostartfiles -static -T user/lib/newlib.ld $(NEWLIB_LIB)

build/crt0.o: user/lib/crt0.c | $(BUILD)
	$(USER_CC) $(NEWLIB_CFLAGS) -c $< -o $@

build/syscalls.o: user/lib/syscalls.c user/lib/embk_syscall.h | $(BUILD)
	$(USER_CC) $(NEWLIB_CFLAGS) -c $< -o $@

build/hello.o: user/bin/hello.c user/lib/embk.h | $(BUILD)
	$(USER_CC) $(NEWLIB_CFLAGS) -c $< -o $@

build/hello.elf: build/crt0.o build/syscalls.o build/hello.o user/lib/newlib.ld
	$(USER_CC) $(NEWLIB_LDFLAGS) build/crt0.o build/syscalls.o build/hello.o -lc -lgcc -o $@



# QEMU drive args: boot disk (master) + data disk (slave)
DRIVES = -drive format=raw,file=$(IMG),if=ide,index=0 \
         -drive format=raw,file=$(DISK),if=ide,index=1

all: $(IMG)

$(STAGE1_BIN): $(STAGE1_SRC) $(STAGE2_BIN)
	@stage2_sectors=$$(( ($$(stat -c%s $(STAGE2_BIN)) + 511) / 512 )); \
	echo "Building stage1 with STAGE2_LOAD_SECTORS=$$stage2_sectors"; \
	$(ASM) $(ASM_FLAGS) -D STAGE2_LOAD_SECTORS=$$stage2_sectors $< -o $@

$(STAGE2_BIN): $(STAGE2_SRC) $(KERNEL_ELF)
	@kernel_sectors=$$(( ($$(stat -c%s $(KERNEL_ELF)) + 511) / 512 )); \
	echo "Building stage2 with KERNEL_LOAD_SECTORS=$$kernel_sectors"; \
	$(ASM) $(ASM_FLAGS) -D KERNEL_LOAD_SECTORS=$$kernel_sectors $< -o $@

$(BUILD):
	mkdir -p $(BUILD)

$(ISR_OBJ): $(ISR_ASM) | $(BUILD)
	$(ASM) -f elf64 $< -o $@

$(SYSCALL_OBJ): $(SYSCALL_ASM) | $(BUILD)
	$(ASM) -f elf64 $< -o $@

$(KCONTEXT_OBJ): $(KCONTEXT_ASM) | $(BUILD)
	$(ASM) -f elf64 $< -o $@

$(KENTRY_OBJ): $(KENTRY_ASM) | $(BUILD)
	$(ASM) -f elf64 $< -o $@

$(AP_ENTRY_OBJ): $(AP_ENTRY_ASM) | $(BUILD)
	$(ASM) -f elf64 $< -o $@

# Flat binary, NOT linked into the kernel -- an AP starts in 16-bit real
# mode and must execute below 1MB, which the kernel's higher-half ELF is
# not. Embedded as data via the incbin wrapper below instead.
$(AP_TRAMPOLINE_BIN): $(AP_TRAMPOLINE_ASM) | $(BUILD)
	$(ASM) -f bin $< -o $@

$(AP_TRAMPOLINE_BLOB_OBJ): $(AP_TRAMPOLINE_BLOB_ASM) $(AP_TRAMPOLINE_BIN) | $(BUILD)
	$(ASM) -f elf64 $< -o $@

$(KERNEL_ELF): $(KERNEL_SRC) $(ISR_OBJ) $(SYSCALL_OBJ) $(KCONTEXT_OBJ) $(KENTRY_OBJ) $(AP_ENTRY_OBJ) $(AP_TRAMPOLINE_BLOB_OBJ) $(LINKER)
	$(CC) $(CFLAGS) -T $(LINKER) -o $@ $(KERNEL_SRC) $(ISR_OBJ) $(SYSCALL_OBJ) $(KCONTEXT_OBJ) $(KENTRY_OBJ) $(AP_ENTRY_OBJ) $(AP_TRAMPOLINE_BLOB_OBJ)

$(IMG): $(STAGE1_BIN) $(STAGE2_BIN) $(KERNEL_ELF)
	cat $(STAGE1_BIN) $(STAGE2_BIN) $(KERNEL_ELF) > $(IMG)
	truncate -s 1M $(IMG)
	@kernel_sectors=$$(( ($$(stat -c%s $(KERNEL_ELF)) + 511) / 512 )); \
	echo "Kernel is $$kernel_sectors sectors; stage2 loads exact size"
# Create the 64 MB data disk only if it doesn't already exist
$(DISK):
	dd if=/dev/zero of=$(DISK) bs=1M count=64

# One recipe, two outputs. & tells GNU Make (4.3+) this recipe produces BOTH
# targets in one run, rather than potentially invoking the script twice if
# both are requested stale in the same `make` invocation.
embkfs.img embkfs_tree.img &: tools/embkfs_mkfs/mkfs_embkfs.py build/init.elf build/hello.elf
	python3 tools/embkfs_mkfs/mkfs_embkfs.py


# Create a 64MB AHCI test disk (separate from disk.img)
ahci.img:
	dd if=/dev/zero of=ahci.img bs=1M count=64

fat32.img:
	dd if=/dev/zero of=fat32.img bs=1M count=64
	mkfs.vfat -F 32 -n EMBLINK fat32.img
	echo "Hello from EmbLink filesystem!" > /tmp/hello.txt
	mcopy -i fat32.img /tmp/hello.txt ::HELLO.TXT
	echo "second file for testing the directory walk" > /tmp/test.txt
	mcopy -i fat32.img /tmp/test.txt ::TEST.TXT
	mmd -i fat32.img ::SUBDIR
	echo "file inside a subdirectory" > /tmp/sub.txt
	mcopy -i fat32.img /tmp/sub.txt ::SUBDIR/INSIDE.TXT


embkfs.img:
	python3 tools/embkfs_mkfs/mkfs_embkfs.py   # adjust path if mkfs writes elsewhere


embkfs_tree.img:
	python3 tools/embkfs_mkfs/mkfs_embkfs.py     # writes embkfs.img AND embkfs_tree.img


# COW mutates the disk — boot a PRISTINE copy each run, then grade it.
EMBKFS_MASTER  := embkfs_tree.img       # pristine, never written by QEMU
EMBKFS_SCRATCH := embkfs_scratch.img

run-embkfs-cow: $(IMG) $(DISK) $(EMBKFS_MASTER)
	cp -f $(EMBKFS_MASTER) $(EMBKFS_SCRATCH)
	qemu-system-x86_64 \
	    -drive format=raw,file=$(IMG),if=ide,index=0 \
	    -drive format=raw,file=$(EMBKFS_SCRATCH),if=ide,index=1 \
		-vga virtio -serial stdio -no-reboot -no-shutdown -m 4G -smp 4
	@echo "--- grading the post-COW image ---"
	python3 embkfs_mkfs/verify_embkfs.py $(EMBKFS_SCRATCH)


run-embkfs-tree: $(IMG) $(DISK) embkfs_tree.img
	qemu-system-x86_64 \
	    -drive format=raw,file=$(IMG),if=ide,index=0 \
	    -drive format=raw,file=embkfs_tree.img,if=ide,index=1 \
	    -serial stdio -no-reboot -no-shutdown


run-embkfs: $(IMG) $(DISK) embkfs.img
	qemu-system-x86_64 \
	    -drive format=raw,file=$(IMG),if=ide,index=0 \
	    -drive format=raw,file=embkfs.img,if=ide,index=1 \
	    -serial stdio -no-reboot -no-shutdown

# Encrypted EMBKFS test volume (v2.2 Phase 4). Passphrase is the fixed test
# string "correcthorsebattery" -- NEVER a real credential, just a KAT-style
# fixture so this target is scriptable. Boots straight to the kernel's
# mount-time passphrase prompt; type it at the keyboard (masked echo).
embkfs_encrypted.img: tools/embkfs_mkfs/mkfs_embkfs.py build/init.elf
	python3 tools/embkfs_mkfs/mkfs_embkfs.py --encrypted embkfs_encrypted.img correcthorsebattery

run-embkfs-encrypted: $(IMG) $(DISK) embkfs_encrypted.img
	qemu-system-x86_64 \
	    -drive format=raw,file=$(IMG),if=ide,index=0 \
	    -drive format=raw,file=embkfs_encrypted.img,if=ide,index=1 \
	    -serial stdio -no-reboot -no-shutdown


run-ahci: $(IMG) $(DISK) ahci.img
	qemu-system-x86_64 \
	    -drive format=raw,file=$(IMG),if=ide,index=0 \
	    -drive format=raw,file=$(DISK),if=ide,index=1 \
	    -device ahci,id=ahci0 \
	    -drive id=satadisk,file=ahci.img,format=raw,if=none \
	    -device ide-hd,drive=satadisk,bus=ahci0.0 \
	    -serial stdio -no-reboot -no-shutdown

run-fat: $(IMG) $(DISK) fat32.img
	qemu-system-x86_64 \
	    -drive format=raw,file=$(IMG),if=ide,index=0 \
	    -drive format=raw,file=$(DISK),if=ide,index=1 \
	    -drive format=raw,file=fat32.img,if=ide,index=2 \
	    -serial stdio -no-reboot -no-shutdown

# ---- Partition-table tests (MBR) -------------------------------------------
# A whole disk carrying an MBR with one primary partition starting at LBA 2048.
# The block layer should expose it as sdb1 and mount the filesystem THERE, not
# on the raw disk. Two flavours: FAT32 and EMBKFS inside the partition.

# 64MB disk, one 63MB FAT32 partition at LBA 2048.
part_fat.img:
	dd if=/dev/zero of=part_fat.img bs=1M count=64 status=none
	printf 'label: dos\nstart=2048, type=0c\n' | sfdisk part_fat.img
	dd if=/dev/zero of=/tmp/fatpart.img bs=512 count=129024 status=none
	mkfs.vfat -F 32 -n PARTTEST /tmp/fatpart.img
	echo "hello from a FAT32 partition" > /tmp/pf.txt
	mcopy -i /tmp/fatpart.img /tmp/pf.txt ::PART.TXT
	dd if=/tmp/fatpart.img of=part_fat.img bs=512 seek=2048 conv=notrunc status=none

# 16MB disk, one partition at LBA 2048 holding the pristine EMBKFS tree image.
part_embkfs.img: embkfs_tree.img
	dd if=/dev/zero of=part_embkfs.img bs=1M count=16 status=none
	printf 'label: dos\nstart=2048, type=83\n' | sfdisk part_embkfs.img
	dd if=embkfs_tree.img of=part_embkfs.img bs=512 seek=2048 conv=notrunc status=none

run-part-fat: $(IMG) $(DISK) part_fat.img
	qemu-system-x86_64 \
	    -drive format=raw,file=$(IMG),if=ide,index=0 \
	    -drive format=raw,file=part_fat.img,if=ide,index=1 \
	    -serial stdio -no-reboot -no-shutdown

run-part-embkfs: $(IMG) $(DISK) part_embkfs.img
	qemu-system-x86_64 \
	    -drive format=raw,file=$(IMG),if=ide,index=0 \
	    -drive format=raw,file=part_embkfs.img,if=ide,index=1,cache=writethrough \
	    -serial stdio -no-reboot -no-shutdown

run: $(IMG) $(DISK)
	qemu-system-x86_64 $(DRIVES) -serial stdio -no-reboot -no-shutdown

# ---- Display / GPU test targets ---------------------------------------------
# stdvga exposes the Bochs DISPI interface -> bochs_vbe.c does a runtime
# modeset on the LFB. virtio-vga boots via its VBE compat layer, then
# virtio_gpu.c takes over with an accelerated guest-memory scan-out.
run-vga-std: $(IMG) $(DISK)
	qemu-system-x86_64 $(DRIVES) -vga std -serial stdio -no-reboot -no-shutdown

run-virtio-gpu: $(IMG) $(DISK)
	qemu-system-x86_64 $(DRIVES) -vga virtio -serial stdio -no-reboot -no-shutdown

# ---- USB host-controller test targets ----------------------------------------
# One target per HC generation. usb-kbd is a full/low-speed HID device;
# usb-storage is high-speed capable (exercises EHCI bulk).
USB_STORAGE_IMG = usbdisk.img
$(USB_STORAGE_IMG):
	dd if=/dev/zero of=$(USB_STORAGE_IMG) bs=1M count=16

run-usb-uhci: $(IMG) $(DISK) $(USB_STORAGE_IMG)
	qemu-system-x86_64 $(DRIVES) \
	    -device piix3-usb-uhci,id=uhci \
	    -device usb-kbd,bus=uhci.0 \
	    -drive id=usbstick,file=$(USB_STORAGE_IMG),format=raw,if=none \
	    -device usb-storage,bus=uhci.0,drive=usbstick \
	    -serial stdio -no-reboot -no-shutdown

run-usb-ohci: $(IMG) $(DISK) $(USB_STORAGE_IMG)
	qemu-system-x86_64 $(DRIVES) \
	    -device pci-ohci,id=ohci \
	    -device usb-kbd,bus=ohci.0 \
	    -drive id=usbstick,file=$(USB_STORAGE_IMG),format=raw,if=none \
	    -device usb-storage,bus=ohci.0,drive=usbstick \
	    -serial stdio -no-reboot -no-shutdown

run-usb-ehci: $(IMG) $(DISK) $(USB_STORAGE_IMG)
	qemu-system-x86_64 $(DRIVES) \
	    -device usb-ehci,id=ehci \
	    -drive id=usbstick,file=$(USB_STORAGE_IMG),format=raw,if=none \
	    -device usb-storage,bus=ehci.0,drive=usbstick \
	    -serial stdio -no-reboot -no-shutdown

run-usb-xhci: $(IMG) $(DISK) $(USB_STORAGE_IMG)
	qemu-system-x86_64 $(DRIVES) \
	    -device qemu-xhci,id=xhci \
	    -device usb-kbd,bus=xhci.0 \
	    -serial stdio -no-reboot -no-shutdown

# EMBKFS-formatted USB mass-storage image, exercised specifically over xHCI
# (its own separate MSC implementation, distinct from usb_core.c's UHCI/
# OHCI/EHCI path) -- proves EMBKFS mounts over USB, not just ATA/AHCI.
usbdisk_embkfs.img: tools/embkfs_mkfs/mkfs_embkfs.py build/init.elf
	python3 tools/embkfs_mkfs/mkfs_embkfs.py usbdisk_embkfs.img

run-usb-embkfs: $(IMG) $(DISK) usbdisk_embkfs.img
	qemu-system-x86_64 $(DRIVES) \
	    -device qemu-xhci,id=xhci \
	    -drive id=usbembkfs,file=usbdisk_embkfs.img,format=raw,if=none \
	    -device usb-storage,bus=xhci.0,drive=usbembkfs \
	    -serial stdio -no-reboot -no-shutdown

# Two independent EMBKFS volumes mounted at once (sdb -> "/", sdc -> "/sdc"),
# both on plain IDE -- exercises embkfs_init()'s multi-volume mount table
# without needing USB. Pair with `test embkfs multivol` at the shell.
run-multivol: $(IMG) $(DISK) embkfs.img usbdisk_embkfs.img
	qemu-system-x86_64 \
	    -drive format=raw,file=$(IMG),if=ide,index=0 \
	    -drive format=raw,file=embkfs.img,if=ide,index=1 \
	    -drive format=raw,file=usbdisk_embkfs.img,if=ide,index=2 \
	    -serial stdio -no-reboot -no-shutdown

debug: $(IMG) $(DISK)
	qemu-system-x86_64 $(DRIVES) -serial stdio -no-reboot -no-shutdown -s -S

run-smp: $(IMG) $(DISK)
	qemu-system-x86_64 $(DRIVES) -serial stdio -no-reboot -no-shutdown -smp 4

run-bigmem: $(IMG) $(DISK)
	qemu-system-x86_64 $(DRIVES) -serial stdio -no-reboot -no-shutdown -m 4G

run-kvm: $(IMG) $(DISK)
	qemu-system-x86_64 -enable-kvm -cpu host -smp 4 $(DRIVES) -serial stdio -no-reboot -no-shutdown

run-all: $(IMG) ahci.img fat32.img
	qemu-system-x86_64 \
	    -drive format=raw,file=$(IMG),if=ide,index=0 \
	    -drive format=raw,file=fat32.img,if=ide,index=1 \
	    -device ahci,id=ahci0 \
	    -drive id=satadisk,file=ahci.img,format=raw,if=none \
	    -device ide-hd,drive=satadisk,bus=ahci0.0 \
	    -serial stdio -no-reboot -no-shutdown

clean:
	rm -f $(STAGE1_BIN) $(STAGE2_BIN) $(KERNEL_ELF) $(IMG)
	rm -rf $(BUILD)

.PHONY: all run debug clean run-smp run-bigmem run-kvm run-ahci run-fat run-all run-embkfs run-embkfs-tree run-embkfs-cow run-part-fat run-part-embkfs run-usb-embkfs run-multivol run-embkfs-encrypted