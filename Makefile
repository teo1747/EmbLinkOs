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
             kernel/drivers/input/mouse.c \
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
             kernel/fs/epfs.c \
             kernel/ipc/handle.c \
             kernel/ipc/channel.c \
             kernel/ipc/endpoint.c \
             kernel/ipc/pipe.c \
             kernel/kworker/kworker.c \
             kernel/gfx/surface.c \
             kernel/gfx/compositor.c \
             kernel/lib/kstring.c \
             kernel/lib/errno.c \
             kernel/lib/kprintf.c

LINKER      = kernel/linker.ld
STAGE1_BIN  = boot/stage1/boot.bin
STAGE2_BIN  = boot/stage2/stage2.bin
KERNEL_ELF  = kernel/kernel.elf
# Stripped copy that actually goes into the boot image. The full kernel.elf
# carries ~490 KB of .debug_*/.symtab that stage2's real-mode loader would
# otherwise stream into 0x10000.. -- past 0xA0000 (video RAM) at sector 1152
# and into the VGA option-ROM window at sector 1408, where int 0x13 writes
# fail (-> disk_error hang) or scribble the VESA framebuffer vbe_init just set.
# strip removes the non-allocated sections; PT_LOAD offsets and e_entry are
# untouched, so load_elf still parses it identically. Keep kernel.elf (with
# symbols) for gdb. See boot ceiling note in memory.
STRIP       = x86_64-elf-strip
KERNEL_BIN  = kernel/kernel.strip.elf


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

# Dynamic-link flags (Phase 2): an ET_EXEC app that imports the toolkit from
# libembk.so. No -static / no -T (default script emits the dynamic sections).
# libembk.so must appear on the link line BEFORE -lc -lm (so ld pulls the libc
# fns the .so needs INTO the app; --export-dynamic re-exports them to the .so);
# --no-dynamic-linker: the kernel is the loader; --hash-style=sysv: DT_HASH.
NEWLIB_DYN_LDFLAGS = -nostartfiles -no-pie $(NEWLIB_LIB)
NEWLIB_DYN_WL      = -Wl,--export-dynamic -Wl,--no-dynamic-linker -Wl,--hash-style=sysv

build/crt0.o: user/lib/crt0.c | $(BUILD)
	$(USER_CC) $(NEWLIB_CFLAGS) -c $< -o $@

build/syscalls.o: user/lib/syscalls.c user/lib/embk_syscall.h | $(BUILD)
	$(USER_CC) $(NEWLIB_CFLAGS) -c $< -o $@

build/hello.o: user/bin/hello.c user/lib/embk.h | $(BUILD)
	$(USER_CC) $(NEWLIB_CFLAGS) -c $< -o $@

build/hello.elf: build/crt0.o build/syscalls.o build/hello.o user/lib/newlib.ld
	$(USER_CC) $(NEWLIB_LDFLAGS) build/crt0.o build/syscalls.o build/hello.o -lc -lgcc -o $@

# --- uidemo.elf: the EmbLink UI toolkit running live in ring-3 -----------------
# The ui/ toolkit (host-tested pure userland C) built for the OS: newlib libc +
# libm + malloc, SSE on. It creates a surface, renders the settings card with
# the CPU backend + TrueType rasteriser, and presents it (sys_ui_present).
UIDEMO_UISRC = ui/scene/scene.c ui/backend/cpu_backend.c ui/backend/font.c \
               ui/backend/scene_render.c ui/layout/layout.c ui/reactive/reactive.c \
               ui/declare/declare.c ui/theme/theme.c ui/kit/kit.c
UIDEMO_UIOBJ = $(patsubst ui/%.c,$(BUILD)/uiobj_%.o,$(subst /,_,$(UIDEMO_UISRC)))
UIDEMO_INC   = -Iui/scene -Iui/backend -Iui/layout -Iui/reactive -Iui/declare -Iui/theme -Iui/kit -Iui/dsl

# compile each toolkit source once for the newlib target
$(BUILD)/uiobj_scene_scene.o: ui/scene/scene.c | $(BUILD)
	$(USER_CC) $(NEWLIB_CFLAGS) $(UIDEMO_INC) -c $< -o $@
$(BUILD)/uiobj_backend_cpu_backend.o: ui/backend/cpu_backend.c | $(BUILD)
	$(USER_CC) $(NEWLIB_CFLAGS) $(UIDEMO_INC) -c $< -o $@
$(BUILD)/uiobj_backend_font.o: ui/backend/font.c | $(BUILD)
	$(USER_CC) $(NEWLIB_CFLAGS) $(UIDEMO_INC) -c $< -o $@
$(BUILD)/uiobj_backend_scene_render.o: ui/backend/scene_render.c | $(BUILD)
	$(USER_CC) $(NEWLIB_CFLAGS) $(UIDEMO_INC) -c $< -o $@
$(BUILD)/uiobj_layout_layout.o: ui/layout/layout.c | $(BUILD)
	$(USER_CC) $(NEWLIB_CFLAGS) $(UIDEMO_INC) -c $< -o $@
$(BUILD)/uiobj_reactive_reactive.o: ui/reactive/reactive.c | $(BUILD)
	$(USER_CC) $(NEWLIB_CFLAGS) $(UIDEMO_INC) -c $< -o $@
$(BUILD)/uiobj_declare_declare.o: ui/declare/declare.c | $(BUILD)
	$(USER_CC) $(NEWLIB_CFLAGS) $(UIDEMO_INC) -c $< -o $@
$(BUILD)/uiobj_theme_theme.o: ui/theme/theme.c | $(BUILD)
	$(USER_CC) $(NEWLIB_CFLAGS) $(UIDEMO_INC) -c $< -o $@
$(BUILD)/uiobj_kit_kit.o: ui/kit/kit.c | $(BUILD)
	$(USER_CC) $(NEWLIB_CFLAGS) $(UIDEMO_INC) -c $< -o $@
$(BUILD)/uiobj_dsl_em.o: ui/dsl/em.c | $(BUILD)
	$(USER_CC) $(NEWLIB_CFLAGS) $(UIDEMO_INC) -c $< -o $@
UIDEMO_OBJS = $(BUILD)/uiobj_scene_scene.o $(BUILD)/uiobj_backend_cpu_backend.o \
              $(BUILD)/uiobj_backend_font.o $(BUILD)/uiobj_backend_scene_render.o \
              $(BUILD)/uiobj_layout_layout.o $(BUILD)/uiobj_reactive_reactive.o \
              $(BUILD)/uiobj_declare_declare.o $(BUILD)/uiobj_theme_theme.o $(BUILD)/uiobj_kit_kit.o \
              $(BUILD)/uiobj_dsl_em.o

# --- libembk.so: the shared UI toolkit + DSL (Phase 2 dynamic linking) ---------
# The SAME toolkit sources, compiled -fPIC and linked into ONE ET_DYN shared
# object. Apps stay ET_EXEC with static libc but import the toolkit from here;
# the kernel's in-kernel dynamic loader (elf.c) resolves the app's toolkit
# imports to libembk.so's exports AND libembk.so's libc imports back to the app's
# --export-dynamic'd static libc. newlib is non-PIC so libc CANNOT live in the
# .so -- it stays in each app; only our own toolkit code is shared. ld -shared
# (not the gcc driver, whose -shared spec is wrong for this bare x86_64-elf
# target) leaves libc symbols UND for load-time binding.
$(BUILD)/picobj_scene_scene.o: ui/scene/scene.c | $(BUILD)
	$(USER_CC) $(NEWLIB_CFLAGS) -fPIC $(UIDEMO_INC) -c $< -o $@
$(BUILD)/picobj_backend_cpu_backend.o: ui/backend/cpu_backend.c | $(BUILD)
	$(USER_CC) $(NEWLIB_CFLAGS) -fPIC $(UIDEMO_INC) -c $< -o $@
$(BUILD)/picobj_backend_font.o: ui/backend/font.c | $(BUILD)
	$(USER_CC) $(NEWLIB_CFLAGS) -fPIC $(UIDEMO_INC) -c $< -o $@
$(BUILD)/picobj_backend_scene_render.o: ui/backend/scene_render.c | $(BUILD)
	$(USER_CC) $(NEWLIB_CFLAGS) -fPIC $(UIDEMO_INC) -c $< -o $@
$(BUILD)/picobj_layout_layout.o: ui/layout/layout.c | $(BUILD)
	$(USER_CC) $(NEWLIB_CFLAGS) -fPIC $(UIDEMO_INC) -c $< -o $@
$(BUILD)/picobj_reactive_reactive.o: ui/reactive/reactive.c | $(BUILD)
	$(USER_CC) $(NEWLIB_CFLAGS) -fPIC $(UIDEMO_INC) -c $< -o $@
$(BUILD)/picobj_declare_declare.o: ui/declare/declare.c | $(BUILD)
	$(USER_CC) $(NEWLIB_CFLAGS) -fPIC $(UIDEMO_INC) -c $< -o $@
$(BUILD)/picobj_theme_theme.o: ui/theme/theme.c | $(BUILD)
	$(USER_CC) $(NEWLIB_CFLAGS) -fPIC $(UIDEMO_INC) -c $< -o $@
$(BUILD)/picobj_kit_kit.o: ui/kit/kit.c | $(BUILD)
	$(USER_CC) $(NEWLIB_CFLAGS) -fPIC $(UIDEMO_INC) -c $< -o $@
$(BUILD)/picobj_dsl_em.o: ui/dsl/em.c | $(BUILD)
	$(USER_CC) $(NEWLIB_CFLAGS) -fPIC $(UIDEMO_INC) -c $< -o $@
$(BUILD)/picobj_dsl_em_app.o: ui/dsl/em_app.c | $(BUILD)
	$(USER_CC) $(NEWLIB_CFLAGS) -fPIC $(UIDEMO_INC) -c $< -o $@
LIBEMBK_OBJS = $(BUILD)/picobj_scene_scene.o $(BUILD)/picobj_backend_cpu_backend.o \
               $(BUILD)/picobj_backend_font.o $(BUILD)/picobj_backend_scene_render.o \
               $(BUILD)/picobj_layout_layout.o $(BUILD)/picobj_reactive_reactive.o \
               $(BUILD)/picobj_declare_declare.o $(BUILD)/picobj_theme_theme.o $(BUILD)/picobj_kit_kit.o \
               $(BUILD)/picobj_dsl_em.o $(BUILD)/picobj_dsl_em_app.o

build/libembk.so: $(LIBEMBK_OBJS)
	$(USER_LD) -shared -soname libembk.so --hash-style=sysv $(LIBEMBK_OBJS) -o $@

libembk: build/libembk.so
	@echo "libembk.so OK"

# --- EmUI apps: AUTO-DISCOVERED from user/bin/*.c -----------------------------
# Every user/bin/*.c EXCEPT the two special-linked programs (init.c is
# freestanding; hello.c is static-newlib) is a dynamically-linked EmUI app,
# built by the identical pattern below and packed onto the boot image
# automatically. So adding a new app is just: drop user/bin/foo.c in and
# `make embkfs.img` -- no per-app Makefile rule, no mkfs edit. (uidemo,
# wmdemo, home, v4demo, clockw all previously had five copies of the same
# two rules here; this replaces them.)
EMUI_APP_SRCS := $(filter-out user/bin/init.c user/bin/hello.c, $(wildcard user/bin/*.c))
EMUI_APPS     := $(patsubst user/bin/%.c,build/%.elf,$(EMUI_APP_SRCS))

# One compile rule for any EmUI app object (newlib CFLAGS + the toolkit
# include paths).
build/%.o: user/bin/%.c user/lib/embk.h | $(BUILD)
	$(USER_CC) $(NEWLIB_CFLAGS) $(UIDEMO_INC) -c $< -o $@

# One DYNAMIC link rule for any EmUI app: it imports the toolkit from
# libembk.so instead of statically bundling it. NO -static (would forbid the
# .so), NO -T newlib.ld (the default script emits the .dynamic/.dynsym/
# .rela.plt/.got the in-kernel loader needs); libembk.so comes BEFORE -lc -lm
# so ld pulls the libc the .so needs INTO the app and --export-dynamic exports
# it back to the .so. --no-dynamic-linker: the kernel is the loader (no
# PT_INTERP); --hash-style=sysv: DT_HASH for symcount.
build/%.elf: build/%.o build/crt0.o build/syscalls.o build/libembk.so
	$(USER_CC) $(NEWLIB_DYN_LDFLAGS) build/crt0.o build/syscalls.o $< \
	    build/libembk.so -lc -lm -lgcc $(NEWLIB_DYN_WL) -o $@

# Build every discovered app.
emui-apps: $(EMUI_APPS)
	@echo "EmUI apps: $(notdir $(EMUI_APPS))"

# Back-compat convenience targets (the pattern rule builds each .elf).
uidemo: build/uidemo.elf
	@echo "uidemo OK"

wmdemo: build/wmdemo.elf
	@echo "wmdemo OK"

home: build/home.elf
	@echo "home OK"



# QEMU drive args: boot disk (master) + data disk (slave)
DRIVES = -drive format=raw,file=$(IMG),if=ide,index=0 \
         -drive format=raw,file=$(DISK),if=ide,index=1

all: $(IMG)

$(STAGE1_BIN): $(STAGE1_SRC) $(STAGE2_BIN)
	@stage2_sectors=$$(( ($$(stat -c%s $(STAGE2_BIN)) + 511) / 512 )); \
	echo "Building stage1 with STAGE2_LOAD_SECTORS=$$stage2_sectors"; \
	$(ASM) $(ASM_FLAGS) -D STAGE2_LOAD_SECTORS=$$stage2_sectors $< -o $@

$(STAGE2_BIN): $(STAGE2_SRC) $(KERNEL_BIN)
	@kernel_sectors=$$(( ($$(stat -c%s $(KERNEL_BIN)) + 511) / 512 )); \
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

# Boot image carries the stripped kernel (see KERNEL_BIN note above).
$(KERNEL_BIN): $(KERNEL_ELF)
	$(STRIP) --strip-all -o $@ $<

$(IMG): $(STAGE1_BIN) $(STAGE2_BIN) $(KERNEL_BIN)
	cat $(STAGE1_BIN) $(STAGE2_BIN) $(KERNEL_BIN) > $(IMG)
	truncate -s 1M $(IMG)
	@kernel_sectors=$$(( ($$(stat -c%s $(KERNEL_BIN)) + 511) / 512 )); \
	video_ceiling=$$(( (0xA0000 - 0x10000) / 512 )); \
	echo "Kernel is $$kernel_sectors sectors (video-RAM ceiling $$video_ceiling); stage2 loads exact size"; \
	if [ $$kernel_sectors -ge $$video_ceiling ]; then \
	  echo "*** WARNING: stripped kernel exceeds the 0xA0000 real-mode load ceiling;"; \
	  echo "*** stage2 will overflow into video RAM/option-ROM. Move to an unreal-mode"; \
	  echo "*** high load (>1 MB) before the kernel grows further."; \
	fi
# Create the 64 MB data disk only if it doesn't already exist
$(DISK):
	dd if=/dev/zero of=$(DISK) bs=1M count=64

# One recipe, two outputs. & tells GNU Make (4.3+) this recipe produces BOTH
# targets in one run, rather than potentially invoking the script twice if
# both are requested stale in the same `make` invocation.
embkfs.img embkfs_tree.img &: tools/embkfs_mkfs/mkfs_embkfs.py build/init.elf build/hello.elf $(EMUI_APPS) build/libembk.so
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
EMBKFS_MASTER  := embkfs.img       # pristine, never written by QEMU
EMBKFS_SCRATCH := embkfs_scratch.img

# Display settings shared by the interactive targets:
#  - XRES/YRES: the virtio-gpu scanout size (the whole stack -- fb, compositor,
#    mouse clamp, home launcher -- adapts to whatever the device reports).
#    Override per run: `make run-embkfs-cow XRES=1920 YRES=1080`.
#  - zoom-to-fit=off: show guest pixels 1:1 instead of stretching them to the
#    window (stretching is what made the display look blurry/badly scaled).
XRES ?= 1280
YRES ?= 800
VGA_VIRTIO = -vga none -device virtio-vga,xres=$(XRES),yres=$(YRES)
DISPLAY_1TO1 = -display gtk,zoom-to-fit=off

run-embkfs-cow: $(IMG) $(DISK) $(EMBKFS_MASTER)
	cp -f $(EMBKFS_MASTER) $(EMBKFS_SCRATCH)
	qemu-system-x86_64 \
	    -drive format=raw,file=$(IMG),if=ide,index=0 \
	    -drive format=raw,file=$(EMBKFS_SCRATCH),if=ide,index=1 \
	    -usb -device usb-tablet \
	    $(VGA_VIRTIO) $(DISPLAY_1TO1) \
	    -serial stdio -no-reboot -no-shutdown -m 521m -smp 1 -accel tcg,thread=multi
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

# --- run-ui: boot to a window, then the live EmbLink UI app ------------------
# Boots EmbLinkOS with a real display; uidemo.elf (the ring-3 UI toolkit, font
# embedded) is on the EMBKFS root. At the shell prompt, type:  run /uidemo.elf
# and the settings card renders into a Piece-1 surface and presents to screen.
#
# Uses virtio-gpu (-vga virtio): its damage-rect scan-out flush presents from a
# cached-RAM backbuffer, avoiding the per-present memcpy into UNCACHED VRAM that
# -vga std incurs (fb_front is vmm_map_mmio'd VMM_NOCACHE) -- ~2x FPS on the
# toolkit demos. Use `make run-vga-std` for the plain uncached-LFB path.
run-ui: $(IMG) embkfs.img
	@echo "At the shell prompt, type:  run /uidemo.elf"
	qemu-system-x86_64 \
	    -drive format=raw,file=$(IMG),if=ide,index=0 \
	    -drive format=raw,file=embkfs.img,if=ide,index=1 \
	    -usb -device usb-tablet \
	    $(VGA_VIRTIO) $(DISPLAY_1TO1) \
	    -serial stdio -no-reboot -no-shutdown -m 512M -m 4G -smp 4

# Boots to the window-compositor demo: two kernel-composited windows (one
# hosting the EmUI toolkit, one drawn directly) over a desktop with title-bar
# chrome + z-order. At the shell prompt, type:  run /wmdemo.elf   (ESC quits).
run-wm: $(IMG) embkfs.img
	@echo "At the shell prompt, type:  run /wmdemo.elf   (ESC quits)"
	qemu-system-x86_64 \
	    -drive format=raw,file=$(IMG),if=ide,index=0 \
	    -drive format=raw,file=embkfs.img,if=ide,index=1 \
	    -usb -device usb-tablet \
	    $(VGA_VIRTIO) $(DISPLAY_1TO1) \
	    -serial stdio -no-reboot -no-shutdown -m 512M -m 4G -smp 4

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
	qemu-system-x86_64 $(DRIVES) -usb -device usb-tablet -serial stdio -no-reboot -no-shutdown

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

# EmbLink UI Piece 3: the scene tree is pure userland C with no syscalls, so
# its selftests are host-compiled and run natively (no QEMU / no ring-3) --
# fast, and appropriate for a pure data-structure + traversal piece.
HOSTCC ?= cc
scene-test:
	$(HOSTCC) -std=c11 -Wall -Wextra -O2 -Iui/scene \
	    ui/scene/scene.c ui/scene/scene_test.c -o $(BUILD)/scene_test
	$(BUILD)/scene_test

# Piece 4a: the CPU render backend + dirty-rect driver. Same host-test posture
# as Piece 3 -- operates on in-memory render_target buffers, no QEMU/ring-3.
backend-test:
	$(HOSTCC) -std=c11 -Wall -Wextra -O2 -Iui/scene -Iui/backend \
	    ui/scene/scene.c ui/backend/cpu_backend.c ui/backend/scene_render.c \
	    ui/backend/backend_test.c -lm -o $(BUILD)/backend_test
	$(BUILD)/backend_test

# Piece 4b: the TrueType font rasterizer. Synthetic-font unit tests, host-run.
font-test:
	$(HOSTCC) -std=c11 -Wall -Wextra -O2 -Iui/scene -Iui/backend \
	    ui/scene/scene.c ui/backend/cpu_backend.c ui/backend/font.c \
	    ui/backend/font_test.c -lm -o $(BUILD)/font_test
	$(BUILD)/font_test

# Piece 5: the flexbox-style layout engine. Host-run unit tests.
layout-test:
	$(HOSTCC) -std=c11 -Wall -Wextra -O2 -Iui/scene -Iui/backend -Iui/layout \
	    ui/scene/scene.c ui/backend/cpu_backend.c ui/backend/font.c \
	    ui/layout/layout.c ui/layout/layout_test.c -lm -o $(BUILD)/layout_test
	$(BUILD)/layout_test

# Piece 6: the reactivity system (signals/scopes/edges). UI-agnostic, host-run.
reactive-test:
	$(HOSTCC) -std=c11 -Wall -Wextra -O2 -Iui/reactive \
	    ui/reactive/reactive.c ui/reactive/reactive_test.c -o $(BUILD)/reactive_test
	$(BUILD)/reactive_test

# Piece 7: the declarative API (capstone). Ties scene/layout/reactive together.
declare-test:
	$(HOSTCC) -std=c11 -Wall -Wextra -O2 -Iui/scene -Iui/backend -Iui/layout -Iui/reactive -Iui/declare \
	    ui/scene/scene.c ui/backend/cpu_backend.c ui/backend/font.c ui/layout/layout.c \
	    ui/reactive/reactive.c ui/declare/declare.c ui/declare/declare_test.c \
	    -lm -o $(BUILD)/declare_test
	$(BUILD)/declare_test

# Themed showcase: renders a real UI with the widget kit to a PNG, so the
# toolkit's actual look can be seen (not just unit-tested).
UI_SRC = ui/scene/scene.c ui/backend/cpu_backend.c ui/backend/font.c ui/backend/scene_render.c \
         ui/layout/layout.c ui/reactive/reactive.c ui/declare/declare.c ui/theme/theme.c ui/kit/kit.c
UI_INC = -Iui/scene -Iui/backend -Iui/layout -Iui/reactive -Iui/declare -Iui/theme -Iui/kit
showcase:
	$(HOSTCC) -std=c11 -Wall -Wextra -O2 $(UI_INC) \
	    $(UI_SRC) ui/showcase/showcase.c -lm -o $(BUILD)/showcase
	$(BUILD)/showcase $(BUILD)/showcase_light.ppm light
	$(BUILD)/showcase $(BUILD)/showcase_dark.ppm  dark
	python3 -c "from PIL import Image; \
	  Image.open('$(BUILD)/showcase_light.ppm').save('$(BUILD)/showcase_light.png'); \
	  Image.open('$(BUILD)/showcase_dark.ppm').save('$(BUILD)/showcase_dark.png'); \
	  print('wrote $(BUILD)/showcase_light.png + showcase_dark.png')"

# EmUI V2 showcase: renders an app screen written in the SwiftUI-flavored DSL.
V2_SRC = $(UI_SRC) ui/dsl/em.c
V2_INC = $(UI_INC) -Iui/dsl
showcase-v2:
	$(HOSTCC) -std=gnu11 -Wall -Wextra -O2 $(V2_INC) \
	    $(V2_SRC) ui/dsl/showcase_v2.c -lm -o $(BUILD)/showcase_v2
	$(BUILD)/showcase_v2 $(BUILD)/v2_light.ppm light
	$(BUILD)/showcase_v2 $(BUILD)/v2_dark.ppm  dark
	$(BUILD)/showcase_v2 $(BUILD)/v4_light.ppm light v4
	$(BUILD)/showcase_v2 $(BUILD)/v4_dark.ppm  dark  v4
	$(BUILD)/showcase_v2 $(BUILD)/v6_light.ppm light 6
	$(BUILD)/showcase_v2 $(BUILD)/v6_dark.ppm  dark  6
	$(BUILD)/showcase_v2 $(BUILD)/v7_light.ppm light 7
	$(BUILD)/showcase_v2 $(BUILD)/v7_dark.ppm  dark  7
	python3 -c "from PIL import Image; \
	  Image.open('$(BUILD)/v2_light.ppm').save('$(BUILD)/v2_light.png'); \
	  Image.open('$(BUILD)/v2_dark.ppm').save('$(BUILD)/v2_dark.png'); \
	  Image.open('$(BUILD)/v4_light.ppm').save('$(BUILD)/v4_light.png'); \
	  Image.open('$(BUILD)/v4_dark.ppm').save('$(BUILD)/v4_dark.png'); \
	  Image.open('$(BUILD)/v6_light.ppm').save('$(BUILD)/v6_light.png'); \
	  Image.open('$(BUILD)/v6_dark.ppm').save('$(BUILD)/v6_dark.png'); \
	  Image.open('$(BUILD)/v7_light.ppm').save('$(BUILD)/v7_light.png'); \
	  Image.open('$(BUILD)/v7_dark.ppm').save('$(BUILD)/v7_dark.png'); \
	  print('wrote v2_{light,dark}.png + v4_{light,dark}.png')"

clean:
	rm -f $(STAGE1_BIN) $(STAGE2_BIN) $(KERNEL_ELF) $(KERNEL_BIN) $(IMG)
	rm -rf $(BUILD)

.PHONY: all run debug clean scene-test backend-test font-test layout-test reactive-test declare-test showcase run-ui run-smp run-bigmem run-kvm run-ahci run-fat run-all run-embkfs run-embkfs-tree run-embkfs-cow run-part-fat run-part-embkfs run-usb-embkfs run-multivol run-embkfs-encrypted