# Tools
ASM      = nasm
ASM_FLAGS = -f bin
CC = x86_64-elf-gcc
CFLAGS = -ffreestanding -nostdlib -nostartfiles \
         -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
         -mcmodel=kernel \
         -g -O0

# Output
IMG = myos.img

# Sources
STAGE1_SRC  = boot/stage1/boot.asm
STAGE2_SRC  = boot/stage2/stage2.asm
ISR_SRC     = kernel/cpu/isr.asm
KERNEL_SRC = kernel/main.c \
             kernel/cpu/isr.c \
			 kernel/cpu/pic.c \
			 kernel/cpu/irq.c \
			 kernel/cpu/gdt.c \
			 kernel/cpu/spinlock.c \
             kernel/drivers/serial.c \
             kernel/drivers/framebuffer.c \
             kernel/drivers/font_8x16.c \
             kernel/drivers/console.c \
             kernel/drivers/timer.c \
             kernel/drivers/keyboard.c \
             kernel/mm/pmm.c \
             kernel/cpu/idt.c \
             kernel/mm/vmm.c \
			 kernel/mm/kheap.c \
             kernel/kprintf.c
ISR_ASM     = kernel/cpu/isr.asm			 
ISR_OBJ     = kernel/cpu/isr.o

LINKER      = kernel/linker.ld

# Binaries
STAGE1_BIN  = boot/stage1/boot.bin
STAGE2_BIN  = boot/stage2/stage2.bin
KERNEL_ELF  = kernel/kernel.elf

# Default target
all: $(IMG)

# Assemble stage 1
$(STAGE1_BIN): $(STAGE1_SRC)
	$(ASM) $(ASM_FLAGS) $< -o $@

# Assemble stage 2
$(STAGE2_BIN): $(STAGE2_SRC)
	$(ASM) $(ASM_FLAGS) $< -o $@

# Assemble ISR stubs
$(ISR_OBJ): $(ISR_ASM)
	$(ASM) -f elf64 $< -o $@

# Compile kernel
$(KERNEL_ELF): $(KERNEL_SRC) $(ISR_OBJ) $(LINKER)
	$(CC) $(CFLAGS) -T $(LINKER) -o $@ $(KERNEL_SRC) $(ISR_OBJ)

# Combine into disk image
$(IMG): $(STAGE1_BIN) $(STAGE2_BIN) $(KERNEL_ELF)
	cat $(STAGE1_BIN) $(STAGE2_BIN) $(KERNEL_ELF) > $(IMG)
	truncate -s 1M $(IMG)

# Run in QEMU
#run: $(IMG)
#	qemu-system-x86_64 -drive format=raw,file=$(IMG) -serial stdio -no-reboot -no-shutdown
run: $(IMG)
	qemu-system-x86_64 -drive format=raw,file=$(IMG) -serial stdio -d int -D qemu.log

# Debug mode
debug: $(IMG)
	qemu-system-x86_64 -drive format=raw,file=$(IMG) \
	    -serial stdio -no-reboot -no-shutdown \
	    -s -S

# Clean build artifacts
clean:
	rm -f $(STAGE1_BIN) $(STAGE2_BIN) $(KERNEL_ELF) $(IMG)

.PHONY: all run debug clean