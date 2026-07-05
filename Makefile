ASM       = nasm
ASM_FLAGS = -f bin
CC = x86_64-elf-gcc
CFLAGS = -ffreestanding -nostdlib -nostartfiles \
         -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
         -mcmodel=kernel \
         -g -O0

IMG = myos.img
DISK = disk.img

# Userland rules appear before `all`, so pin the default goal explicitly.
.DEFAULT_GOAL := all

STAGE1_SRC  = boot/stage1/boot.asm
STAGE2_SRC  = boot/stage2/stage2.asm
ISR_ASM     = kernel/cpu/isr.asm
KCONTEXT_ASM = kernel/cpu/kcontext.asm
KENTRY_ASM  = kernel/cpu/kentry.asm
KENTRY_OBJ  = kernel/cpu/kentry.o
ISR_OBJ     = kernel/cpu/isr.o
SYSCALL_ASM = kernel/cpu/syscall_entry.asm
SYSCALL_OBJ = kernel/cpu/syscall_entry.o
KCONTEXT_OBJ = kernel/cpu/kcontext.o

KERNEL_SRC = kernel/main.c \
             kernel/cpu/isr.c \
             kernel/cpu/pic.c \
             kernel/cpu/irq.c \
             kernel/cpu/gdt.c \
             kernel/cpu/spinlock.c \
             kernel/cpu/rwlock.c \
             kernel/cpu/syscall.c \
             kernel/cpu/usercopy.c \
             kernel/cpu/lapic.c \
             kernel/cpu/ioapic.c \
			 kernel/cpu/usermode.c \
			 kernel/cpu/elf.c \
			 kernel/process/process.c \
             kernel/acpi/acpi.c \
             kernel/drivers/serial.c \
             kernel/drivers/framebuffer.c \
             kernel/drivers/gpu.c \
             kernel/drivers/bochs_vbe.c \
             kernel/drivers/virtio_gpu.c \
             kernel/drivers/font_8x16.c \
             kernel/drivers/console.c \
             kernel/drivers/timer.c \
             kernel/drivers/hpet.c \
             kernel/drivers/keyboard.c \
             kernel/drivers/pit.c \
             kernel/drivers/pci.c \
             kernel/drivers/usb.c \
             kernel/drivers/usb_core.c \
             kernel/drivers/xhci.c \
             kernel/drivers/ehci.c \
             kernel/drivers/uhci.c \
             kernel/drivers/ohci.c \
             kernel/drivers/ata.c \
             kernel/drivers/bootanim.c \
             kernel/drivers/ahci.c \
             kernel/block/block.c \
             kernel/block/partition.c \
             kernel/mm/pmm.c \
             kernel/cpu/idt.c \
             kernel/mm/vmm.c \
             kernel/mm/kheap.c \
             kernel/mm/kmalloc.c \
             kernel/fs/fat32.c \
			 kernel/fs/embkfs/embkfs.c \
			 kernel/fs/embkfs/crc32c.c \
			 kernel/fs/embkfs/embk_vfs.c \
			 kernel/fs/fd.c \
			 kernel/fs/vfs.c \
			 kernel/selftests.c \
             kernel/kstring.c \
             kernel/errno.c \
             kernel/kprintf.c

LINKER      = kernel/linker.ld
STAGE1_BIN  = boot/stage1/boot.bin
STAGE2_BIN  = boot/stage2/stage2.bin
KERNEL_ELF  = kernel/kernel.elf


# ---- Userland ---------------------------------------------------------------
# The first program that isn't the kernel. Freestanding, linked at 0x400000,
# flattened to a raw blob, then embedded into the kernel image (see init_blob).
USER_CC      = x86_64-elf-gcc
USER_LD      = x86_64-elf-ld
USER_OBJCOPY = x86_64-elf-objcopy
USER_CFLAGS  = -ffreestanding -nostdlib -fno-pic -mno-red-zone \
               -fno-stack-protector -mno-mmx -mno-sse -mno-sse2 -O2

user/init.o: user/init.c
	$(USER_CC) $(USER_CFLAGS) -c $< -o $@


user/init.elf: user/init.o user/user.ld
	$(USER_LD) -T user/user.ld user/init.o -o $@

user/init.bin: user/init.elf
	$(USER_OBJCOPY) -O binary $< $@

# Wrap the raw blob in an object the kernel links against. The .incbin pulls in
# the bytes; the two labels bracket them so C can compute the length at runtime.
user/init_blob.o: user/init.elf kernel/cpu/init_blob.asm
	$(ASM) -f elf64 kernel/cpu/init_blob.asm -o $@



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

$(ISR_OBJ): $(ISR_ASM)
	$(ASM) -f elf64 $< -o $@

$(SYSCALL_OBJ): $(SYSCALL_ASM)
	$(ASM) -f elf64 $< -o $@

$(KCONTEXT_OBJ): $(KCONTEXT_ASM)
	$(ASM) -f elf64 $< -o $@

$(KENTRY_OBJ): $(KENTRY_ASM)
	$(ASM) -f elf64 $< -o $@

$(KERNEL_ELF): $(KERNEL_SRC) $(ISR_OBJ) $(SYSCALL_OBJ) $(KCONTEXT_OBJ) $(KENTRY_OBJ) $(LINKER)
	$(CC) $(CFLAGS) -T $(LINKER) -o $@ $(KERNEL_SRC) $(ISR_OBJ) $(SYSCALL_OBJ) $(KCONTEXT_OBJ) $(KENTRY_OBJ)

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
embkfs.img embkfs_tree.img &: embkfs_mkfs/mkfs_embkfs.py user/init.elf
	python3 embkfs_mkfs/mkfs_embkfs.py


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
	python3 embkfs_mkfs/mkfs_embkfs.py   # adjust path if mkfs writes elsewhere


embkfs_tree.img:
	python3 embkfs_mkfs/mkfs_embkfs.py     # writes embkfs.img AND embkfs_tree.img


# COW mutates the disk — boot a PRISTINE copy each run, then grade it.
EMBKFS_MASTER  := embkfs_tree.img       # pristine, never written by QEMU
EMBKFS_SCRATCH := embkfs_scratch.img

run-embkfs-cow: $(IMG) $(DISK) $(EMBKFS_MASTER)
	cp -f $(EMBKFS_MASTER) $(EMBKFS_SCRATCH)
	qemu-system-x86_64 \
	    -drive format=raw,file=$(IMG),if=ide,index=0 \
	    -drive format=raw,file=$(EMBKFS_SCRATCH),if=ide,index=1 \
		-vga virtio -serial stdio -no-reboot -no-shutdown -m 4G
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
	rm -f $(STAGE1_BIN) $(STAGE2_BIN) $(KERNEL_ELF) $(IMG) \
	      $(ISR_OBJ) $(SYSCALL_OBJ) $(KCONTEXT_OBJ) $(KENTRY_OBJ)\
	      user/init.o user/init.elf user/init.bin user/init_blob.o

.PHONY: all run debug clean run-smp run-bigmem run-kvm run-ahci run-fat run-all run-embkfs run-embkfs-tree run-embkfs-cow run-part-fat run-part-embkfs