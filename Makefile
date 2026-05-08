# Compiler and assembler
ASM = nasm
ASM_FLAGS = -f bin

# Output
IMG = myos.img

# Sources
STAGE1_SRC = boot/stage1/boot.asm
STAGE2_SRC = boot/stage2/stage2.asm

# Binaries
STAGE1_BIN = boot/stage1/boot.bin
STAGE2_BIN = boot/stage2/stage2.bin

# Default target
all: $(IMG)

# Assemble stage 1
$(STAGE1_BIN): $(STAGE1_SRC)
	$(ASM) $(ASM_FLAGS) $< -o $@

# Assemble stage 2
$(STAGE2_BIN): $(STAGE2_SRC)
	$(ASM) $(ASM_FLAGS) $< -o $@

# Combine into disk image
$(IMG): $(STAGE1_BIN) $(STAGE2_BIN)
	cat $(STAGE1_BIN) $(STAGE2_BIN) > $(IMG)

# Run in QEMU
run: $(IMG)
	qemu-system-x86_64 -drive format=raw,file=$(IMG) -no-reboot -no-shutdown

# Debug mode
debug: $(IMG)
	qemu-system-x86_64 -drive format=raw,file=$(IMG) -no-reboot -no-shutdown -d int,cpu_reset

# Clean build artifacts
clean:
	rm -f $(STAGE1_BIN) $(STAGE2_BIN) $(IMG)

.PHONY: all run debug clean