ASM       = nasm
ASM_FLAGS = -f bin
CC = x86_64-elf-gcc
CFLAGS = -ffreestanding -nostdlib -nostartfiles \
         -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
         -mcmodel=kernel \
         -g -O0

IMG = myos.img
DISK = disk.img

STAGE1_SRC  = boot/stage1/boot.asm
STAGE2_SRC  = boot/stage2/stage2.asm
ISR_ASM     = kernel/cpu/isr.asm
ISR_OBJ     = kernel/cpu/isr.o

KERNEL_SRC = kernel/main.c \
             kernel/cpu/isr.c \
             kernel/cpu/pic.c \
             kernel/cpu/irq.c \
             kernel/cpu/gdt.c \
             kernel/cpu/spinlock.c \
             kernel/cpu/lapic.c \
             kernel/cpu/ioapic.c \
             kernel/acpi/acpi.c \
             kernel/drivers/serial.c \
             kernel/drivers/framebuffer.c \
             kernel/drivers/font_8x16.c \
             kernel/drivers/console.c \
             kernel/drivers/timer.c \
             kernel/drivers/keyboard.c \
             kernel/drivers/pit.c \
             kernel/drivers/pci.c \
             kernel/drivers/ata.c \
             kernel/drivers/bootanim.c \
             kernel/drivers/ahci.c \
             kernel/block/block.c \
             kernel/mm/pmm.c \
             kernel/cpu/idt.c \
             kernel/mm/vmm.c \
             kernel/mm/kheap.c \
             kernel/kstring.c \
             kernel/errno.c \
             kernel/kprintf.c

LINKER      = kernel/linker.ld
STAGE1_BIN  = boot/stage1/boot.bin
STAGE2_BIN  = boot/stage2/stage2.bin
KERNEL_ELF  = kernel/kernel.elf

# QEMU drive args: boot disk (master) + data disk (slave)
DRIVES = -drive format=raw,file=$(IMG),if=ide,index=0 \
         -drive format=raw,file=$(DISK),if=ide,index=1

all: $(IMG)

$(STAGE1_BIN): $(STAGE1_SRC)
	$(ASM) $(ASM_FLAGS) $< -o $@

$(STAGE2_BIN): $(STAGE2_SRC)
	$(ASM) $(ASM_FLAGS) $< -o $@

$(ISR_OBJ): $(ISR_ASM)
	$(ASM) -f elf64 $< -o $@

$(KERNEL_ELF): $(KERNEL_SRC) $(ISR_OBJ) $(LINKER)
	$(CC) $(CFLAGS) -T $(LINKER) -o $@ $(KERNEL_SRC) $(ISR_OBJ)

$(IMG): $(STAGE1_BIN) $(STAGE2_BIN) $(KERNEL_ELF)
	cat $(STAGE1_BIN) $(STAGE2_BIN) $(KERNEL_ELF) > $(IMG)
	truncate -s 1M $(IMG)
	@kernel_sectors=$$(( ($$(stat -c%s $(KERNEL_ELF)) + 511) / 512 )); \
	echo "Kernel is $$kernel_sectors sectors; stage2 loads 512"; \
	if [ $$kernel_sectors -gt 512 ]; then \
	    echo "*** WARNING: kernel exceeds stage2 load size! Bump cx in stage2.asm ***"; \
	fi
# Create the 64 MB data disk only if it doesn't already exist
$(DISK):
	dd if=/dev/zero of=$(DISK) bs=1M count=64

# Create a 64MB AHCI test disk (separate from disk.img)
ahci.img:
	dd if=/dev/zero of=ahci.img bs=1M count=64

run-ahci: $(IMG) $(DISK) ahci.img
	qemu-system-x86_64 \
	    -drive format=raw,file=$(IMG),if=ide,index=0 \
	    -drive format=raw,file=$(DISK),if=ide,index=1 \
	    -device ahci,id=ahci0 \
	    -drive id=satadisk,file=ahci.img,format=raw,if=none \
	    -device ide-hd,drive=satadisk,bus=ahci0.0 \
	    -serial stdio -no-reboot -no-shutdown

run: $(IMG) $(DISK)
	qemu-system-x86_64 $(DRIVES) -serial stdio -no-reboot -no-shutdown

debug: $(IMG) $(DISK)
	qemu-system-x86_64 $(DRIVES) -serial stdio -no-reboot -no-shutdown -s -S

run-smp: $(IMG) $(DISK)
	qemu-system-x86_64 $(DRIVES) -serial stdio -no-reboot -no-shutdown -smp 4

run-bigmem: $(IMG) $(DISK)
	qemu-system-x86_64 $(DRIVES) -serial stdio -no-reboot -no-shutdown -m 4G

run-kvm: $(IMG) $(DISK)
	qemu-system-x86_64 -enable-kvm -cpu host -smp 4 $(DRIVES) -serial stdio -no-reboot -no-shutdown

clean:
	rm -f $(STAGE1_BIN) $(STAGE2_BIN) $(KERNEL_ELF) $(IMG) $(ISR_OBJ)

.PHONY: all run debug clean run-smp run-bigmem run-kvm run-ahci