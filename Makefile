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
             kernel/process/ksync.c \
			 kernel/tty/tty.c \
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
ASM_GAS      = x86_64-elf-as   # GNU as for the dynstubs (.set/.weak)
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

# Let scripts ask the Makefile where newlib is, so NEWLIB_PREFIX stays the ONE
# source of truth and tools/tcc/build-tcc-emblink.sh cannot drift from the build.
.PHONY: print-newlib-prefix print-cxx-prefix
print-newlib-prefix:
	@echo '$(NEWLIB_PREFIX)'
print-cxx-prefix:
	@echo '$(if $(HAVE_CXX),$(CXX_PREFIX),)'
NEWLIB_INC    = $(if $(NEWLIB_PREFIX),-isystem $(NEWLIB_PREFIX)/x86_64-elf/include,)
NEWLIB_LIB    = $(if $(NEWLIB_PREFIX),-L$(NEWLIB_PREFIX)/x86_64-elf/lib,)
NEWLIB_CFLAGS = -mno-red-zone -fno-stack-protector -O2 -Wall $(USER_INC) $(NEWLIB_INC)
# gcc as the link driver so it finds libc.a/libgcc; -nostartfiles because
# crt0.c provides _start (no standard crtX). newlib.ld places it at 0x400000.
# NEWLIB_LIB is a -L searched BEFORE the toolchain's default lib dir, so our
# rebuilt libc.a wins over the stock one.
NEWLIB_LDFLAGS = -nostartfiles -static -T user/lib/newlib.ld $(NEWLIB_LIB)

# --- C++ (optional toolchain) -------------------------------------------------
# The stock /usr/local/cross gcc is `--enable-languages=c` only, so there is no
# x86_64-elf-g++ and no libstdc++. CXX_PREFIX points at a SECOND, user-owned
# toolchain built with c,c++ + libstdc++ against the SAME newlib the apps link
# (tools: /home/motsou/cross/build_gcc_cxx.sh) -- exactly the NEWLIB_PREFIX
# pattern above: no sudo, and the stock C toolchain stays untouched.
#
# Everything C++ is GATED on that compiler existing, so the tree still builds
# fine without it -- `make cxx-check` reports whether it's available.
# Constructors are already handled: crt0.c walks .init_array AND .ctors.
CXX_PREFIX ?= /home/motsou/cross/gcc-cxx
USER_CXX    = $(CXX_PREFIX)/bin/x86_64-elf-g++
HAVE_CXX   := $(if $(wildcard $(USER_CXX)),yes,)

# -fno-exceptions/-fno-rtti for now: both want unwind tables + a personality
# routine wired into the runtime, which is its own piece of work. Plain C++
# (classes, templates, ctors, new/delete) needs neither.
CXXFLAGS_EMBK = -mno-red-zone -fno-stack-protector -O2 -Wall \
                -fno-exceptions -fno-rtti $(USER_INC) $(NEWLIB_INC)

cxx-check:
	@if [ -n "$(HAVE_CXX)" ]; then \
	    echo "C++: $(USER_CXX)"; $(USER_CXX) -dumpversion; \
	    echo -n "libstdc++: "; ls $(CXX_PREFIX)/x86_64-elf/lib/libstdc++.a 2>/dev/null || echo "MISSING"; \
	else \
	    echo "C++: not built. Run /home/motsou/cross/build_gcc_cxx.sh"; \
	fi
.PHONY: cxx-check

# cxxdemo.elf -- the first C++ program (user/bin/cxxdemo.cc). A .cc extension,
# so the user/bin/*.c EmUI auto-discovery leaves it alone. Statically linked
# like shell.elf; the g++ driver pulls in libstdc++/libsupc++ (which
# function-local statics need for __cxa_guard_acquire). CXX_APPS is EMPTY when
# no C++ toolchain is installed, so the whole tree still builds without one.
ifeq ($(HAVE_CXX),yes)
CXX_APPS = build/cxxdemo.elf

build/cxxdemo.o: user/bin/cxxdemo.cc | $(BUILD)
	$(USER_CXX) $(CXXFLAGS_EMBK) -c $< -o $@

build/cxxdemo.elf: build/cxxdemo.o build/crt0.o build/syscalls.o user/lib/newlib.ld
	$(USER_CXX) $(NEWLIB_LDFLAGS) build/crt0.o build/syscalls.o build/cxxdemo.o \
	    -lstdc++ -lsupc++ -lc -lgcc -o $@
else
CXX_APPS =
endif

# --- CPython (optional, built OUT OF TREE) ------------------------------------
# The interpreter is cross-built in /home/motsou/cross/build-py (configure with
# /home/motsou/cross/configure-py-emblink.sh -- it encodes the platform patches,
# the config.site and the crt0/syscalls link line). Same gate shape as HAVE_CXX
# above: absent toolchain == PY_APPS empty == the tree still builds.
#
# It links against build/crt0.o + build/syscalls.o, so the OS must be built once
# before configuring CPython -- and a libc change means reconfiguring it.
PY_BUILD ?= /home/motsou/cross/build-py
PY_BIN   := $(PY_BUILD)/python
HAVE_PY  := $(if $(wildcard $(PY_BIN)),yes,)

# git, same shape as HAVE_PY: absent cross-built binary == GIT_APPS empty ==
# the tree still builds. Build it with /home/motsou/cross/build-git-emblink.sh.
# The build script now lives IN THE REPO (tools/git/), so a teammate's checkout
# can rebuild git without a copy of the author's home directory. It takes the
# source tree as an argument and needs a cross-built zlib -- both overridable.
GIT_SRC  ?= /home/motsou/cross/git-2.49.1
GIT_BIN  ?= $(GIT_SRC)/git
ZLIB_BUILD ?= /home/motsou/cross/build-zlib
GIT_BUILD_SH ?= $(CURDIR)/tools/git/build-git-emblink.sh

# TCC -- a C compiler that RUNS ON the OS. Chosen because it is compiler +
# assembler + linker in ONE binary: EmbLink has no exec, so GCC's driver (which
# fork/execs cc1, as, ld) is structurally impossible here, while `tcc a.c -o a`
# is a single process start to finish.
TCC_SRC ?= /home/motsou/cross/tcc-0.9.27
TCC_BIN ?= $(TCC_SRC)/tcc
HAVE_TCC := $(if $(wildcard $(TCC_BIN)),yes,)
HAVE_GIT := $(if $(wildcard $(GIT_BIN)),yes,)

PY_SRC   ?= /home/motsou/cross/Python-3.14.6

ifeq ($(HAVE_PY),yes)
# The interpreter, the stdlib, and the file that points one at the other. All
# three or none: python.elf without the zip dies at startup with
# "Failed to import encodings module" (encodings is imported by Py_Initialize
# and is NOT frozen), and the zip without the ._pth is never looked at.
PY_APPS = build/python.elf build/python314.zip build/python.elf._pth

# STRIP IS NOT COSMETIC HERE: the interpreter is 42 MB unstripped (almost all of
# it debug_info) and 7.5 MB stripped. The 64 MB volume would not hold the former
# alongside cxxdemo.elf's 9.4 MB. Keep the unstripped original in $(PY_BUILD) --
# that's what addr2line needs when the thing faults.
# DEPENDS ON THE LIBC, and that is the whole point of this rule's complexity:
# python EMBEDS build/crt0.o + build/syscalls.o (they are in its LDFLAGS). But
# CPython's own Makefile has them as RAW PATHS, not prerequisites, so its `make`
# reports "up to date" however much our libc changed -- it cannot know. That is
# not hypothetical: python.elf once sat 15 HOURS stale, passing `test python`
# against a frozen copy of an old libc. Green, and true of a binary nobody
# builds any more.
#
# So WE track it. Relink only when the libc is genuinely NEWER than the
# interpreter -- a blind `rm && make` would relink on every single build.
build/python.elf: $(PY_BIN) build/crt0.o build/syscalls.o | $(BUILD)
	@if [ build/syscalls.o -nt $(PY_BIN) ] || [ build/crt0.o -nt $(PY_BIN) ]; then \
	    echo "  RELINK   python  (libc changed under it)"; \
	    rm -f $(PY_BIN); \
	    $(MAKE) -s -C $(PY_BUILD) python || \
	      { echo "*** python relink FAILED -- see $(PY_BUILD)"; exit 1; }; \
	 fi
	cp -f $(PY_BIN) $@
	$(STRIP) $@

# The stdlib as ONE zip: importlib+zipimport are frozen into the interpreter, so
# a zip on sys.path needs no directory tree and no CPython patch (it's CPython's
# own ZIP_LANDMARK route, the standard embedded recipe). ~555 modules, ~2.5 MB.
build/python314.zip: tools/mkpystdlib.py | $(BUILD)
	python3 tools/mkpystdlib.py $(PY_SRC) $@

# getpath.py reads `<executable>._pth` (verbatim on non-Windows, hence the
# `.elf` stays in the name) and its lines TOTALLY override sys.path -- each is
# joinpath()'d onto the file's own directory, so these resolve to /python314.zip
# and /. It also forces isolated mode + no environment lookup, which is exactly
# right for an OS that has no environment. This is why we don't fight configure's
# --prefix, which otherwise sends the interpreter hunting through /usr/....
build/python.elf._pth: | $(BUILD)
	printf 'python314.zip\n.\n' > $@
else
PY_APPS =
endif

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

# --- ON-OS DYNAMIC-LINK ABI (so tcc can build EmUI apps against libembk.so) ---
# emlink_dynstubs.o: the tcc-world equivalent of newlib.ld's PROVIDE()s -- crt0's
# weak bracket symbols (__ctors_start/end, __tls_*) as weak absolute 0. tcc uses
# no linker script and would otherwise turn them into unresolvable dynamic
# imports. libtcc1.o: the float/64-bit intrinsics tcc's codegen EMITS but gcc
# inlines (so they're absent from x86_64 libgcc) -- __floatundisf & friends.
# Cross-built here (both are just as/C); shipped to /system/abi by mkfs.
build/emlink_dynstubs.o: user/lib/emlink_dynstubs.s | $(BUILD)
	$(ASM_GAS) $< -o $@
build/libtcc1.o: $(TCC_SRC)/lib/libtcc1.c | $(BUILD)
	$(USER_CC) -c -O2 -mno-red-zone -fno-stack-protector -ffreestanding $< -o $@

build/hello.o: user/bin/hello.c user/lib/embk.h | $(BUILD)
	$(USER_CC) $(NEWLIB_CFLAGS) -c $< -o $@

build/hello.elf: build/crt0.o build/syscalls.o build/hello.o user/lib/newlib.ld
	$(USER_CC) $(NEWLIB_LDFLAGS) build/crt0.o build/syscalls.o build/hello.o -lc -lgcc -o $@

# posixdemo.elf -- ring-3 self-check for the POSIX layer in user/lib/syscalls.c
# (dirent/clocks/cwd + the honest refusals). `test posix` spawns it. Plain C, so
# unlike cxxdemo it needs no toolchain gate. mkfs auto-discovers build/*.elf.
build/posixdemo.o: user/bin/posixdemo.c | $(BUILD)
	$(USER_CC) $(NEWLIB_CFLAGS) -c $< -o $@

build/posixdemo.elf: build/crt0.o build/syscalls.o build/posixdemo.o user/lib/newlib.ld
	$(USER_CC) $(NEWLIB_LDFLAGS) build/crt0.o build/syscalls.o build/posixdemo.o -lc -lgcc -o $@

# ioracer.elf -- contends for the block layer's shared DMA bounce buffer and
# verifies it got its OWN file's bytes back. Static-newlib console program like
# posixdemo (no UI), spawned N-up by `test blockrace`.
build/ioracer.o: user/bin/ioracer.c | $(BUILD)
	$(USER_CC) $(NEWLIB_CFLAGS) -c $< -o $@

build/ioracer.elf: build/crt0.o build/syscalls.o build/ioracer.o user/lib/newlib.ld
	$(USER_CC) $(NEWLIB_LDFLAGS) build/crt0.o build/syscalls.o build/ioracer.o -lc -lgcc -o $@

# capchild / capspawn -- the ring-1 capability-attenuation test pair. Static
# newlib console programs (no UI). capspawn is spawned by `test spawncaps` with
# a limited cap set and drives the rest across the spawn syscall.
build/capchild.o: user/bin/capchild.c user/lib/embk.h | $(BUILD)
	$(USER_CC) $(NEWLIB_CFLAGS) -c $< -o $@
build/capchild.elf: build/crt0.o build/syscalls.o build/capchild.o user/lib/newlib.ld
	$(USER_CC) $(NEWLIB_LDFLAGS) build/crt0.o build/syscalls.o build/capchild.o -lc -lgcc -o $@
build/capspawn.o: user/bin/capspawn.c user/lib/embk.h | $(BUILD)
	$(USER_CC) $(NEWLIB_CFLAGS) -c $< -o $@
build/capspawn.elf: build/crt0.o build/syscalls.o build/capspawn.o user/lib/newlib.ld
	$(USER_CC) $(NEWLIB_LDFLAGS) build/crt0.o build/syscalls.o build/capspawn.o -lc -lgcc -o $@

# --- shell.elf: the EmbLink structured shell (shell/) --------------------------
# Static newlib link (hello.elf's shape, not the EmUI dynamic one -- the shell
# has no UI dependency). Kernel-convention includes: one -Ishell root,
# subtree-qualified paths ("value/value.h"). docs/SHELL.md is the spec.
SHELL_INC = -Ishell
SHELL_HDRS = shell/value/value.h shell/wire/wire.h shell/sval/sval.h \
             shell/lex/lex.h shell/parse/parse.h shell/eval/eval.h \
             shell/builtins/builtins.h shell/hist/hist.h

$(BUILD)/shobj_value.o: shell/value/value.c $(SHELL_HDRS) | $(BUILD)
	$(USER_CC) $(NEWLIB_CFLAGS) $(SHELL_INC) -c $< -o $@
$(BUILD)/shobj_wire.o: shell/wire/wire.c $(SHELL_HDRS) | $(BUILD)
	$(USER_CC) $(NEWLIB_CFLAGS) $(SHELL_INC) -c $< -o $@
$(BUILD)/shobj_sval.o: shell/sval/sval.c $(SHELL_HDRS) | $(BUILD)
	$(USER_CC) $(NEWLIB_CFLAGS) $(SHELL_INC) -c $< -o $@
$(BUILD)/shobj_lex.o: shell/lex/lex.c $(SHELL_HDRS) | $(BUILD)
	$(USER_CC) $(NEWLIB_CFLAGS) $(SHELL_INC) -c $< -o $@
$(BUILD)/shobj_parse.o: shell/parse/parse.c $(SHELL_HDRS) | $(BUILD)
	$(USER_CC) $(NEWLIB_CFLAGS) $(SHELL_INC) -c $< -o $@
$(BUILD)/shobj_eval.o: shell/eval/eval.c $(SHELL_HDRS) | $(BUILD)
	$(USER_CC) $(NEWLIB_CFLAGS) $(SHELL_INC) -c $< -o $@
$(BUILD)/shobj_builtins.o: shell/builtins/builtins.c $(SHELL_HDRS) | $(BUILD)
	$(USER_CC) $(NEWLIB_CFLAGS) $(SHELL_INC) -c $< -o $@
$(BUILD)/shobj_hist.o: shell/hist/hist.c $(SHELL_HDRS) | $(BUILD)
	$(USER_CC) $(NEWLIB_CFLAGS) $(SHELL_INC) -c $< -o $@
$(BUILD)/shobj_eval_extern.o: shell/eval/eval_extern.c $(SHELL_HDRS) user/lib/embk.h | $(BUILD)
	$(USER_CC) $(NEWLIB_CFLAGS) $(SHELL_INC) -c $< -o $@
$(BUILD)/shobj_builtins_os.o: shell/builtins/builtins_os.c $(SHELL_HDRS) user/lib/embk.h | $(BUILD)
	$(USER_CC) $(NEWLIB_CFLAGS) $(SHELL_INC) -c $< -o $@
$(BUILD)/shobj_main.o: shell/main.c $(SHELL_HDRS) | $(BUILD)
	$(USER_CC) $(NEWLIB_CFLAGS) $(SHELL_INC) -c $< -o $@

SHELL_OBJS = $(BUILD)/shobj_value.o $(BUILD)/shobj_wire.o $(BUILD)/shobj_sval.o \
             $(BUILD)/shobj_lex.o $(BUILD)/shobj_parse.o $(BUILD)/shobj_eval.o \
             $(BUILD)/shobj_builtins.o $(BUILD)/shobj_hist.o \
             $(BUILD)/shobj_eval_extern.o \
             $(BUILD)/shobj_builtins_os.o $(BUILD)/shobj_main.o

build/shell.elf: $(SHELL_OBJS) build/crt0.o build/syscalls.o user/lib/newlib.ld
	$(USER_CC) $(NEWLIB_LDFLAGS) build/crt0.o build/syscalls.o $(SHELL_OBJS) -lc -lgcc -o $@

# --- external pipeline tools (shell/tools/): NOT builtins -- each is a real
# standalone .elf the shell spawns as a pipeline stage via the extern path
# (fd 3 out / fd 0 in). They link the sval SDK's core objects statically,
# same shape as shell.elf. sysinfo = the reference producer; tally = the
# reference consumer (an external re-implementation of `count`).
SHELL_SDK_OBJS = $(BUILD)/shobj_value.o $(BUILD)/shobj_wire.o $(BUILD)/shobj_sval.o

$(BUILD)/shtool_sysinfo.o: shell/tools/sysinfo.c $(SHELL_HDRS) user/lib/embk.h | $(BUILD)
	$(USER_CC) $(NEWLIB_CFLAGS) $(SHELL_INC) -c $< -o $@
$(BUILD)/shtool_tally.o: shell/tools/tally.c $(SHELL_HDRS) | $(BUILD)
	$(USER_CC) $(NEWLIB_CFLAGS) $(SHELL_INC) -c $< -o $@
$(BUILD)/shtool_embbuild.o: shell/tools/embbuild.c $(SHELL_HDRS) user/lib/embk.h | $(BUILD)
	$(USER_CC) $(NEWLIB_CFLAGS) $(SHELL_INC) -c $< -o $@

build/sysinfo.elf: $(BUILD)/shtool_sysinfo.o $(SHELL_SDK_OBJS) build/crt0.o build/syscalls.o user/lib/newlib.ld
	$(USER_CC) $(NEWLIB_LDFLAGS) build/crt0.o build/syscalls.o $< $(SHELL_SDK_OBJS) -lc -lgcc -o $@
build/tally.elf: $(BUILD)/shtool_tally.o $(SHELL_SDK_OBJS) build/crt0.o build/syscalls.o user/lib/newlib.ld
	$(USER_CC) $(NEWLIB_LDFLAGS) build/crt0.o build/syscalls.o $< $(SHELL_SDK_OBJS) -lc -lgcc -o $@
# EmbBuild (docs/BUILD.md): the typed-manifest walker. Same static sval-tool
# shape; host-bootstrapped here, self-hosting is its own target #3.
build/embbuild.elf: $(BUILD)/shtool_embbuild.o $(SHELL_SDK_OBJS) build/crt0.o build/syscalls.o user/lib/newlib.ld
	$(USER_CC) $(NEWLIB_LDFLAGS) build/crt0.o build/syscalls.o $< $(SHELL_SDK_OBJS) -lc -lgcc -o $@

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
# posixdemo.c is filtered out for the same reason as hello.c: it's a plain
# static-newlib console program with its own rule above, NOT an EmUI app to be
# linked against libembk.so.
EMUI_APP_SRCS := $(filter-out user/bin/init.c user/bin/hello.c user/bin/posixdemo.c user/bin/ioracer.c user/bin/capchild.c user/bin/capspawn.c, $(wildcard user/bin/*.c))
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

# `make` builds a COMPLETE, CURRENT system: the boot image AND the userland
# image. embkfs.img was deliberately absent here before, so a plain `make` could
# exit 0 having never built the apps it packs -- `make && make run-*` would then
# boot an image with a stale (or entirely missing) program on it.
all: $(IMG) embkfs.img

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

# Every kernel header is a prerequisite. The kernel is one monolithic $(CC)
# over $(KERNEL_SRC) with no per-TU depfiles, so without this a header-only
# change (a #define in spawn.h, a struct field) leaves kernel.elf "up to date"
# and `make` silently packs the OLD kernel -- a stale-binary trap that cost two
# 35-minute boots during EmbBuild v2. A wildcard over every .h under kernel/ is
# coarse (any header touch rebuilds the whole kernel) but the kernel is one
# compile anyway, so the granularity is already all-or-nothing; correctness wins.
KERNEL_HDRS := $(shell find kernel -name '*.h')
$(KERNEL_ELF): $(KERNEL_SRC) $(KERNEL_HDRS) $(ISR_OBJ) $(SYSCALL_OBJ) $(KCONTEXT_OBJ) $(KENTRY_OBJ) $(AP_ENTRY_OBJ) $(AP_TRAMPOLINE_BLOB_OBJ) $(LINKER)
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

# Everything that must be BUILT before mkfs runs. mkfs itself auto-discovers
# build/*.elf (see discover_userland_objects), so this list exists purely to tell
# make WHEN the image is stale -- and it has to stay complete, because an app
# missing from it is not a build error: the image just silently keeps the old
# copy, or never gains the new app at all. EMUI_APPS is a wildcard over
# user/bin/*.c, so EmUI apps are automatic; static console programs have bespoke
# rules and must be named here (the check in the recipe below catches omissions).
ifeq ($(HAVE_GIT),yes)
GIT_APPS = build/git.elf

# Same reasoning as python.elf: 4.7 MB unstripped, 3.5 MB stripped; keep the
# unstripped original next to the source tree for addr2line when it faults.
# Same libc-embedding problem as python.elf above (see that rule for why git's
# own Makefile cannot catch this): git links build/crt0.o + build/syscalls.o via
# LDFLAGS, so a libc change silently leaves the packed git.elf on the old one.
# Relink only when the libc is actually newer.
build/git.elf: $(GIT_BIN) build/crt0.o build/syscalls.o | $(BUILD)
	@if [ build/syscalls.o -nt $(GIT_BIN) ] || [ build/crt0.o -nt $(GIT_BIN) ]; then \
	    echo "  RELINK   git     (libc changed under it)"; \
	    rm -f $(GIT_BIN); \
	    ZLIB_BUILD=$(ZLIB_BUILD) $(GIT_BUILD_SH) $(GIT_SRC) >/dev/null 2>&1 || \
	      { echo "*** git relink FAILED -- run: ZLIB_BUILD=$(ZLIB_BUILD) $(GIT_BUILD_SH) $(GIT_SRC)"; exit 1; }; \
	 fi
	cp -f $(GIT_BIN) $@
	$(STRIP) $@
endif

ifeq ($(HAVE_TCC),yes)
TCC_APPS = build/tcc.elf

# Same libc-embedding problem as python.elf/git.elf (see those rules): tcc links
# build/crt0.o + build/syscalls.o via LDFLAGS, which its own Makefile treats as
# raw paths. Relink only when the libc is genuinely newer.
build/tcc.elf: $(TCC_BIN) build/crt0.o build/syscalls.o | $(BUILD)
	@if [ build/syscalls.o -nt $(TCC_BIN) ] || [ build/crt0.o -nt $(TCC_BIN) ]; then \
	    echo "  RELINK   tcc     (libc changed under it)"; \
	    rm -f $(TCC_BIN); \
	    $(MAKE) -s -C $(TCC_SRC) CONFIG_ldl=no tcc || \
	      { echo "*** tcc relink FAILED -- see $(TCC_SRC)"; exit 1; }; \
	 fi
	cp -f $(TCC_BIN) $@
	$(STRIP) $@
endif

EMBKFS_APPS := build/init.elf build/hello.elf build/posixdemo.elf build/ioracer.elf \
               build/capchild.elf build/capspawn.elf \
               build/shell.elf build/sysinfo.elf build/tally.elf \
               build/embbuild.elf \
               $(CXX_APPS) $(PY_APPS) $(GIT_APPS) $(TCC_APPS) $(EMUI_APPS)

# STAGED_APPS: binaries built OUTSIDE this tree and dropped into build/ to be
# judged by the machine rather than by their author's host -- e.g. EmbCC (a
# separate repo) stages an object it compiled, linked against our crt0/newlib,
# so the OS can answer "does it actually run?". Empty by default and never
# built here: nothing in this tree may depend on a foreign toolchain existing.
#
# Explicit on the command line, deliberately -- `make STAGED_APPS=build/x.elf`.
# A wildcard would have been shorter and would have silently disarmed the drift
# guard below for EVERY unnamed .elf, which is the exact bug the guard exists to
# catch. Naming the file is the point.
STAGED_APPS ?=

# One recipe, two outputs. & tells GNU Make (4.3+) this recipe produces BOTH
# targets in one run, rather than potentially invoking the script twice if
# both are requested stale in the same `make` invocation.
embkfs.img embkfs_tree.img &: tools/embkfs_mkfs/mkfs_embkfs.py $(EMBKFS_APPS) $(STAGED_APPS) build/libembk.so build/libtcc1.o build/emlink_dynstubs.o
	@# Drift guard: mkfs packs every build/*.elf it finds, but make only knows
	@# about $(EMBKFS_APPS). Anything in the first set and not the second lands
	@# on the image yet never triggers a rebuild -- a stale-image bug that is
	@# otherwise completely silent. Warn loudly rather than let it rot.
	@for f in build/*.elf; do \
	  case " $(EMBKFS_APPS) $(STAGED_APPS) " in \
	    *" $$f "*) ;; \
	    *) echo "*** WARNING: $$f is packed onto the image but is NOT a"; \
	       echo "***          prerequisite of embkfs.img -- changes to it will"; \
	       echo "***          NOT rebuild the image. Add it to EMBKFS_APPS."; ;; \
	  esac; \
	done
	@# EMBK_NEWLIB_LIBC: where mkfs finds the libc.a it packs into /system/abi
	@# for tcc to link against ON the OS. Derived from the ONE NEWLIB_PREFIX so a
	@# checkout on another machine needs no mkfs edit -- see docs/BUILD_SETUP.md.
	@# EMBK_NEWLIB_INC / EMBK_TCC_INC: the HEADER trees mkfs packs so tcc can
	@# COMPILE real #include-ing programs on-OS -- newlib's headers are part of
	@# the ABI (-> /system/abi/include), tcc's own compiler headers
	@# (stddef.h/stdarg.h/...) belong to the compiler (-> /data/apps/tcc/include).
	@# Both paths mirror what build-tcc-emblink.sh bakes into tcc's search paths.
	EMBK_NEWLIB_LIBC="$(if $(NEWLIB_PREFIX),$(NEWLIB_PREFIX)/x86_64-elf/lib/libc.a,)" \
	EMBK_NEWLIB_LIBM="$(if $(NEWLIB_PREFIX),$(NEWLIB_PREFIX)/x86_64-elf/lib/libm.a,)" \
	EMBK_NEWLIB_INC="$(if $(NEWLIB_PREFIX),$(NEWLIB_PREFIX)/x86_64-elf/include,)" \
	EMBK_TCC_INC="$(if $(HAVE_TCC),$(TCC_SRC)/include,)" \
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


# NOTE: embkfs.img / embkfs_tree.img are built by the GROUPED rule further up
# (search "One recipe, two outputs"). Two prerequisite-less duplicates used to
# sit here and, being later, they OVERRODE that rule -- which meant make treated
# an existing embkfs.img as up to date forever: rebuilt apps never reached the
# image and new ones never appeared at all. Don't re-add a bare `embkfs.img:`
# rule; put new prerequisites on the grouped rule instead.


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

# A real GPT disk, written by sfdisk -- the INDEPENDENT oracle. We are not
# grading our own homework: if our reader disagrees with the tool that wrote it,
# our reader is wrong. Three partitions so the entry walk is exercised, not just
# entry 0, and one of them lands past index 9 territory for the name check.
part_gpt.img:
	dd if=/dev/zero of=part_gpt.img bs=1M count=32 status=none
	printf 'label: gpt\nstart=2048, size=8192, type=linux\nstart=10240, size=8192, type=linux\nstart=18432, size=8192, type=linux\n' | sfdisk part_gpt.img

# An MBR disk with an EXTENDED partition holding logical partitions (sda5+).
# The EBR chain is a linked list whose two link fields use DIFFERENT bases --
# the thing most likely to be wrong, so make the chain long enough to catch it.
part_ext.img:
	dd if=/dev/zero of=part_ext.img bs=1M count=32 status=none
	printf 'label: dos\nstart=2048, size=4096, type=83\nstart=6144, size=51200, type=5\nstart=8192, size=8192, type=83\nstart=18432, size=8192, type=83\nstart=28672, size=8192, type=83\n' | sfdisk part_ext.img

run-part-gpt: $(IMG) part_gpt.img
	qemu-system-x86_64 -drive format=raw,file=$(IMG),if=ide,index=0 \
	    -drive format=raw,file=part_gpt.img,if=ide,index=1 \
	    -serial stdio -no-reboot -no-shutdown

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

# Nested-directory fixture (needs no build/ output -- content is inline). Proves
# the formatter builds a multi-level tree the kernel traverses; pair with
# `test dirtree` (resolves /system/bin/hello.txt on the 2nd volume). Prerequisite
# for the /system + /data layout migration (docs/USERSPACE.md).
dirtree.img: tools/embkfs_mkfs/mkfs_embkfs.py
	python3 tools/embkfs_mkfs/mkfs_embkfs.py --dirtree dirtree.img

# Boot the default volume at "/" (index 1) PLUS the nested fixture (index 2 ->
# embkfs_volume_at(1)), so `test dirtree` can resolve nested paths on it.
run-dirtree: $(IMG) $(DISK) embkfs.img dirtree.img
	qemu-system-x86_64 \
	    -drive format=raw,file=$(IMG),if=ide,index=0 \
	    -drive format=raw,file=embkfs.img,if=ide,index=1 \
	    -drive format=raw,file=dirtree.img,if=ide,index=2 \
	    -serial stdio -no-reboot -no-shutdown

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

# --- compile_commands.json: make clangd/clang-tidy resolve this tree ---------
# Nothing here compiles per-file into a database (the kernel is ONE gcc call
# over $(KERNEL_SRC)), so `bear` has nothing to capture and clangd otherwise
# can't find a single project header -- every file shows a wall of bogus
# "file not found" that buries the real diagnostics. The generator mirrors the
# four include worlds below (CFLAGS / NEWLIB_CFLAGS+UIDEMO_INC / SHELL_INC).
# Output is absolute-path'd + gitignored: regenerate, don't commit.
compile-commands:
	python3 tools/gen_compile_commands.py

.PHONY: compile-commands

# The structured shell's pure pipeline (lexer -> parser -> evaluator ->
# builtins + the wire round-trip), host-run. The two OS-backed pieces (ls,
# external spawn) link as stubs; they get exercised live by `test shell`.
shell-test:
	$(HOSTCC) -std=c11 -Wall -Wextra -O1 -g -Ishell \
	    shell/value/value.c shell/wire/wire.c shell/sval/sval.c \
	    shell/lex/lex.c shell/parse/parse.c shell/eval/eval.c \
	    shell/builtins/builtins.c shell/hist/hist.c shell/test/host_stubs.c \
	    shell/test/host_test.c -o $(BUILD)/shell_test
	$(BUILD)/shell_test

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