# Build System, Toolchain & Setup

This is the guide for getting a **fresh clone or fork of this repo building
and booting** on a new machine, plus an explanation of what's actually
happening in three things new contributors trip over: the `x86_64-elf` cross
compiler, the newlib rebuild, and dynamic linking. If you already have
everything set up and just want the list of `make` targets, see
[CONTRIBUTING.md](../CONTRIBUTING.md#build--run) instead — this doc is about
first-time setup and the *why*, not day-to-day usage.

## What you need, and why

| Tool | What it's for |
|---|---|
| `x86_64-elf-gcc`, `-ld`, `-strip` | The cross compiler. Never use your host's `gcc` — it targets your host OS's ABI/libc, not this OS's. |
| `nasm` | Assembles the bootloader and a handful of kernel `.asm` files. |
| `qemu-system-x86_64` | Every `make run-*` target boots here. |
| `gdb` | Source-level debugging via `make debug` + `.gdbinit` (auto-attaches to `:1234`, breaks at `kernel_main`). |
| `python3` | The EMBKFS image builder/verifier (`tools/embkfs_mkfs/`) — a from-scratch, dependency-free filesystem format with a Python reference implementation used both to build test images and to independently verify what the kernel writes. |
| `python3` + Pillow (`pip install pillow` or `python3-pil`) | Only for `make showcase`/`showcase-v2` — converts the EmUI toolkit's rendered PPM output to PNG. Not required for building/booting the OS. |
| `mtools`, `mkfs.vfat`, `sfdisk` | Only for the FAT32 and MBR-partition test targets (`fat32.img`, `part_fat.img`, etc.) — not required for the default build. |

On Debian/Ubuntu, everything except the cross compiler and newlib (below)
is one line:
```
sudo apt install nasm qemu-system-x86 gdb python3 python3-pil mtools dosfstools fdisk make
```

## Building the `x86_64-elf` cross compiler

**There is no script for this in the repo — you build it once, by hand,
using the standard [OSDev cross-compiler
process](https://wiki.osdev.org/GCC_Cross-Compiler).** This is the single
biggest one-time setup cost (30–60 minutes) and the thing most likely to
trip up a first-time contributor, so budget time for it before expecting
`make` to work.

The Makefile assumes the resulting toolchain is on your `PATH` as
`x86_64-elf-gcc` / `x86_64-elf-ld` / `x86_64-elf-strip` (this project's
original setup put it at `/usr/local/cross/bin`, but any location on `PATH`
works — override `USER_CC`/`CC` in the Makefile if you name your binaries
differently).

Standard recipe (binutils, then a **newlib-integrated** GCC — this project's
userland links against newlib, so building GCC *with* newlib support baked
in, rather than a bare `--without-headers` compiler, saves you a second
bootstrap step later):

```bash
export PREFIX="/usr/local/cross"
export TARGET=x86_64-elf
export PATH="$PREFIX/bin:$PATH"

# 1. binutils
cd /tmp && wget https://ftp.gnu.org/gnu/binutils/binutils-2.42.tar.gz
tar xf binutils-2.42.tar.gz && mkdir build-binutils && cd build-binutils
../binutils-2.42/configure --target=$TARGET --prefix="$PREFIX" \
    --with-sysroot --disable-nls --disable-werror
make -j$(nproc) && make install

# 2. newlib source, dropped into the gcc source tree so gcc's build
#    picks it up and produces a toolchain with a working libc from the start
cd /tmp && wget https://ftp.gnu.org/gnu/gcc/gcc-13.2.0/gcc-13.2.0.tar.gz
tar xf gcc-13.2.0.tar.gz
wget ftp://sourceware.org/pub/newlib/newlib-4.4.0.20231231.tar.gz
tar xf newlib-4.4.0.20231231.tar.gz
ln -s ../newlib-4.4.0.20231231/newlib gcc-13.2.0/newlib
ln -s ../newlib-4.4.0.20231231/libgloss gcc-13.2.0/libgloss

# 3. gcc + newlib combined build
mkdir build-gcc && cd build-gcc
../gcc-13.2.0/configure --target=$TARGET --prefix="$PREFIX" \
    --disable-nls --enable-languages=c --without-headers \
    --with-newlib --disable-multilib
make -j$(nproc) all-gcc all-target-libgcc all-target-newlib
make install-gcc install-target-libgcc install-target-newlib
```

Exact versions aren't load-bearing (any recent binutils/gcc/newlib triple
that targets bare `x86_64-elf` works) — what matters is: the target triple
is `x86_64-elf` (a bare-metal ELF target, no OS ABI), and newlib ends up
built into the same prefix so `x86_64-elf-gcc -lc` links out of the box. If
you'd rather not build GCC yourself, some Linux distros package
`x86_64-elf-gcc`/`x86_64-elf-binutils` (Arch/AUR, Homebrew on macOS) — verify
whichever you use actually bundles newlib (`echo 'int main(){}' | x86_64-elf-gcc -x c - -o /tmp/t -lc`
should link without error) before assuming it's usable.

## The newlib-c99 rebuild (why there are two newlibs)

Once you have a working cross compiler, you *could* stop there — but the
newlib that ships baked into a stock cross-compiler build (including the one
built above) is typically configured **without** C99 printf format support:
`%z`, `%j`, `%t`, and `%ll` get silently compiled out of `libc.a`, so
`printf("%zu", some_size_t)` prints the literal string `"zu"` instead of a
number. That's a compile-time config baked into `libc.a` — no flag on the
*calling* side can turn it back on.

The fix used throughout this project's userland is to rebuild **just
newlib** (not the whole cross compiler) into a second, separate prefix with
those options turned on, and point the Makefile at it instead of the
toolchain's built-in one:

```bash
# grab the SAME newlib version your gcc build used (or any newlib source tree)
cd /tmp && tar xf newlib-4.4.0.20231231.tar.gz   # or fetch fresh
mkdir build-newlib-c99 && cd build-newlib-c99
../newlib-4.4.0.20231231/configure --target=x86_64-elf \
    --prefix="$HOME/cross/newlib-c99" \
    --disable-newlib-supplied-syscalls --disable-multilib --disable-nls \
    --enable-newlib-io-c99-formats --enable-newlib-io-long-long
make && make install
```

`--disable-newlib-supplied-syscalls` matters: this project supplies its own
syscall retargeting layer (`user/lib/syscalls.c` — `read`/`write`/`open`/...
implemented against the EmbLink syscall ABI), so newlib's own stub
implementations (which assume a different OS) must be disabled or they'd
conflict at link time.

Point the build at it via `NEWLIB_PREFIX` (Makefile default is
`/home/motsou/cross/newlib-c99` — **this is the original author's home
directory and will not exist on your machine**; override it):

```bash
make NEWLIB_PREFIX=$HOME/cross/newlib-c99 embkfs.img
# or edit the Makefile's NEWLIB_PREFIX line once so you don't retype it
```

To skip this and use the stock toolchain newlib instead (accepting the lost
`%z`/`%ll` support), build with `NEWLIB_PREFIX=` (empty). See
[user/README.md](../user/README.md) for how this plugs into `NEWLIB_CFLAGS`/
`NEWLIB_LIB` at the Makefile level.

## First build

```bash
git clone <your fork/clone url> myos && cd myos
make NEWLIB_PREFIX=$HOME/cross/newlib-c99          # kernel + bootloader -> myos.img
make embkfs.img                                     # builds every userland app + libembk.so,
                                                      # packs them into an EMBKFS disk image
make run-embkfs-cow                                 # boots straight to the graphical home launcher
```
If `NEWLIB_PREFIX` is annoying to keep retyping, set it once at the top of
the Makefile instead of passing it on every invocation. See
[CONTRIBUTING.md](../CONTRIBUTING.md#build--run) for the full list of
`run-*` targets (AHCI, USB variants, encrypted volumes, multi-core, etc.)
and [EMUI_GUIDE.md](EMUI_GUIDE.md) if you're here to write a UI app rather
than hack on the kernel.

## Dynamic linking: how `libembk.so` actually works

This section explains the mechanism, not just the commands — useful if
you're debugging a link/load failure or extending the loader.

### The three ways to link a userland program

| Style | Example | libc | Toolkit |
|---|---|---|---|
| **Freestanding** | `init.elf` | none — own `_start` | n/a |
| **Static newlib** | `hello.elf` | linked into the `.elf` | n/a |
| **Dynamic (EmUI apps)** | `uidemo.elf`, `v4demo.elf`, `home.elf`, ... | still linked into the `.elf` (see below) | imported from `libembk.so` at load time |

Every EmUI app is built the third way. The Makefile rule (grep for
`NEWLIB_DYN_LDFLAGS`/`NEWLIB_DYN_WL`) looks like:

```make
$(USER_CC) $(NEWLIB_DYN_LDFLAGS) build/crt0.o build/syscalls.o build/yourapp.o \
    build/libembk.so -lc -lm -lgcc $(NEWLIB_DYN_WL) -o $@
```
with `NEWLIB_DYN_LDFLAGS = -nostartfiles -no-pie $(NEWLIB_LIB)` and
`NEWLIB_DYN_WL = -Wl,--export-dynamic -Wl,--no-dynamic-linker -Wl,--hash-style=sysv`.

### Why this exists: what it saves

`libembk.so` is the UI toolkit (`ui/scene`, `ui/layout`, `ui/declare`,
`ui/kit`, `ui/theme`, `ui/backend`, `ui/dsl/em.c` + `em_app.c`) compiled
**once**, `-fPIC`, into one shared object
(`ld -shared -soname libembk.so --hash-style=sysv ...` — the `ld` linker
directly, not the `gcc` driver, because gcc's `-shared` spec assumes a hosted
target and produces the wrong thing for bare `x86_64-elf`). Every app that
imports it saves roughly the size of the whole toolkit — apps that used to
statically bundle it were noticeably larger before this shipped.

### Why libc is NOT in the `.so`

newlib, as built above, is **not position-independent** — it wasn't compiled
`-fPIC`. A shared object needs its code to be PIC. So `libembk.so` contains
only this project's own toolkit code (which *is* compiled `-fPIC` for the
`.so` build — see the Makefile's `picobj_*` object rules, a parallel set to
the `uiobj_*` objects used for static/host builds), and libc stays
statically linked into **each app** as normal. The `.so`'s own calls into
libc (`malloc`, `memcpy`, `sinf`, ...) are left as *undefined* symbols in the
`.so` and resolved at load time back against whichever app loaded it — which
only works because every app is linked with `--export-dynamic`, forcing its
static libc's symbols into its own dynamic symbol table so the loader can
find them.

### There is no userspace `ld.so` — the kernel is the loader

This OS has no `PT_INTERP`, no dynamic linker binary living on disk. The
entire dynamic-linking implementation is **in the kernel**, in
[`kernel/arch/x86_64/syscall/elf.c`](../kernel/arch/x86_64/syscall/elf.c),
run once at `exec`/`spawn` time. Roughly, in order:

1. Load the app's own `ET_EXEC` segments at their linked addresses (same as
   the static-linking path).
2. Notice its `PT_DYNAMIC` segment names `DT_NEEDED libembk.so`; load
   `libembk.so`'s segments at a fixed base address (`DYLIB_VA_BASE`) in the
   same address space.
3. Walk both modules' relocations and apply them (`R_X86_64_RELATIVE`,
   `R_X86_64_64`, `R_X86_64_GLOB_DAT`, `R_X86_64_JUMP_SLOT`, and
   `R_X86_64_COPY` for the first-ever cross-object *data* — as opposed to
   function — reference, which needed special handling: the symbol's actual
   storage lives in the app's copy, so the `.so`'s reference has to bind back
   to that, not to a copy inside the `.so`). **Two-way symbol resolution**:
   the app's references to toolkit functions resolve into `libembk.so`; the
   `.so`'s references to libc functions resolve back into the app's own
   (exported) static libc.
4. All relocations are applied **eagerly** at load time — there is no lazy
   PLT binding (no page-fault-driven `.plt` stub resolution). This is
   simpler and correct, at the cost of a slightly slower process start,
   which hasn't mattered in practice yet.

If a spawn fails with an unresolved-symbol or unhandled-relocation-type
error, this is the file to read — `resolve_sym`/`resolve_sym_ex` and
`apply_relocs` are the two functions doing the actual work.

### Adding a new dynamically-linked app

See [EMUI_GUIDE.md's wiring section](EMUI_GUIDE.md#wiring-your-app-into-the-boot-image)
for the concrete Makefile + `mkfs_embkfs.py` steps — mechanically it's "copy
an existing app's three registration points and rename," not something that
needs new loader code.

## Troubleshooting

- **`x86_64-elf-gcc: command not found`** — the cross compiler isn't on
  `PATH`, or wasn't built. See [above](#building-the-x86_64-elf-cross-compiler).
- **`undefined reference to __sfnprintf` / `%zu` prints `zu`** — you're
  linking against the stock toolchain newlib, not the C99-enabled rebuild.
  Set `NEWLIB_PREFIX` (see [above](#the-newlib-c99-rebuild-why-there-are-two-newlibs)).
- **`ELF dynlink: unhandled reloc type` in the serial log at boot** — the
  in-kernel loader hit a relocation kind `apply_relocs()` doesn't handle
  yet; this has come up before for the first-ever cross-object data
  reference in a new app (`R_X86_64_COPY`). Check
  `kernel/arch/x86_64/syscall/elf.c`.
- **A new app doesn't show up after `make embkfs.img`** — it's missing from
  one of the three registration points (Makefile build rule, the image
  dependency line, `mkfs_embkfs.py`'s object list). See
  [EMUI_GUIDE.md](EMUI_GUIDE.md#wiring-your-app-into-the-boot-image).
- **`NEWLIB_PREFIX` points at a path that doesn't exist** — the Makefile's
  default is hardcoded to the original author's home directory. Override it
  on the command line or edit the Makefile once.
