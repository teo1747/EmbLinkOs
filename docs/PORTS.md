# Ports: C++, CPython, git, and a C compiler on the OS

EmbLinkOS runs four substantial pieces of third-party software. This doc says
what each one actually does here, what it honestly cannot do, and why the OS
grew the way it did to host them. For *how to build* them, see
[BUILD_SETUP.md](BUILD_SETUP.md#optional-ports-c-cpython-git-tcc); this doc is
the *what and why*.

None of them is a fork. CPython needed **one** patch and git **zero**; TCC
needed **four**, and each of the four is a fact about this target rather than a
disagreement with tcc — see the section below.

| Port | Status | Proof it works | Needs |
|---|---|---|---|
| C++ / libstdc++ | runs | `test cxx` → 13/13 | — |
| CPython 3.14 | runs | `test python` → "hello from CPython on EmbLink" | `-cpu max` |
| git 2.49 | runs | `test git repo` → a real root commit | `-cpu max` |
| **TCC 0.9.27** | **runs; builds the OS's own tools *and* its GUI apps** | **`test tcc real` → `exit=42` + byte-exact stdio; `test tcc dyn` + `test embbuild gui` → an EmUI widget the OS built itself renders a frame** | — |

`-cpu max` is not a QEMU nicety: `getentropy()` here is RDRAND-backed and
**refuses to fabricate entropy**, returning `ENOSYS` on a CPU without it. Both
CPython and git use it for real (git names temp files with it), so without
RDRAND they fail loudly rather than silently doing something unsafe.

---

## The design constraint everything bends around: there is no `exec`

EmbLink is **not** POSIX. It has `spawn` + file-actions, and no `fork`/`exec`,
by design — see [ARCHITECTURE.md](ARCHITECTURE.md). Every port collides with
that somewhere, and the collision is where the interesting work is.

**This is why the compiler is TCC and not GCC.** GCC is not "harder" here; it is
*structurally impossible*. The `gcc` you type is a **driver** that fork/execs
`cc1`, then `as`, then `ld` as separate programs. Its first act on this OS would
be a spawn it cannot make. TCC is compiler + assembler + linker **in one 368 KB
binary**: `tcc a.c -o a` is a single process from start to finish. Its only
`exec` is the `-m32/-m64` dispatcher, which an x86_64-only build never compiles
in. That was the decisive check before a line of work was done.

The same shape recurs: git's pager is an exec, so `GIT_PAGER=` is required for
`git log`; CPython's `subprocess` is honest `ENOSYS`.

## THE RULE that keeps the ports honest

> A refusal is only honest if the capability is genuinely **absent**.
> Ask: is this *missing*, or *trivially true here*?

This is not philosophy, it is a bug class. `FD_CLOEXEC` was refused with
`ENOSYS` because "we have no exec" — but with no exec, "close this fd on exec"
is **vacuously satisfied**, and accepting it costs nothing. Refusing it broke
*every file open in CPython*. Contrast `fsync`, which returns `ENOSYS` and
should: we cannot prove durability, so promising it would be a lie.

Applied elsewhere: `SA_RESTART=0` for git is honest (we never interrupt a
syscall with a signal at all, so "restart it" is vacuous); `SIGPIPE` being
ignorable is honest (our pipe write already returns `EPIPE`); but `SIGINT` was
a *real* gap, and got real machinery — see [INTERRUPTION.md](INTERRUPTION.md).

---

## What each port taught the OS

Each port is a forcing function. The OS grew these because something real
needed them, not because a checklist said so:

- **CPython** → TLS (`__thread`), relative paths, `fcntl`, and the discovery
  that startup wasn't hanging but was **quadratic I/O** (36000 → ~250 ms/MB).
- **git** → `/dev/null`, **atomic `rename`** (its lockfile protocol depends on
  the victim being torn down in the *same* commit), `ftruncate`, `chmod`, and a
  real `unlink` (whose stub had been a stale lie).
- **TCC** → `libc.a` + both header trees on the image (`/system/abi/include`,
  `/data/apps/tcc/include`), the GCC type-macro predefines (patch 0002), and —
  while proving the tally rebuild — a real shell bug (the extern-consumer EOF
  close) and a real kernel bug (the IF=0 voluntary-block wake leak, ledger
  Bug 26).
- **All of them** → the environment (`getenv`) and per-process **cwd**, both
  passed *explicitly at spawn*: nothing is inherited here, a parent names `PWD`
  and the child's crt0 seeds from it.

## TCC: the OS compiles C, on itself

`test tcc link` compiles, assembles, links, and **runs** a program without ever
leaving the OS:

```
[tcc link] -static -nostdlib /l.c /crt0.o /syscalls.o -L/ -lc -o /l.elf
[tcc link] exit=0
[tcc link] /l.elf: written, 73268 bytes; ELF=1 ET_EXEC=1 phnum=2
[tcc link] /l.elf ran: exit=42 (want 42)
```

`42` is the whole claim: it can only appear if `int main(void){return twice(21);}`
was really compiled and really executed. A valid-looking ELF would prove only
that the writer works; `tcc -v` exiting 0 would prove nothing at all.

That toy was deliberately header-free and pulled **zero** members out of
`libc.a`. `test tcc real` closes the remaining distance — a **real** program,
`#include <stdio.h>`/`<string.h>`/`<stdint.h>`, compiled against the on-image
newlib headers, linked with a bare `-lc` (tcc's baked-in `/system/abi` libpath,
no `-L`, no `-lgcc`), then run:

```
[tcc real] -static -nostdlib /data/tmp/r.c /system/abi/crt0.o /system/abi/syscalls.o -lc -o /data/tmp/r.elf
[tcc real] tcc exit=0
[tcc real] r.elf (92916 bytes) ran: exit=42 (want 42)
[tcc real] r.out: byte-exact (got "hdr=ok n=42")
```

The proof is twofold: the computed exit code, and the file the program wrote
through **buffered stdio** — `fopen`/`fprintf`/`fclose` drag in dozens of
`libc.a` members and hundreds of cross-object calls (exactly the load the
PLT32 patch must survive at scale), and the bytes read back exact. The binary
is the same size as its host-linked twin.

And `test tcc tally` closes the loop: **the OS rebuilds one of its own tools
from source.** mkfs packs tally's exact 7-file closure (the sval SDK it links)
at `/data/src/shell/`; on-OS, tcc compiles the four units (`-c
-I/data/src/shell`, quote-includes resolving exactly as the host build's
`-Ishell`), a **separate invocation links the four tcc-produced objects** —
the compile-then-link shape every build tool assumes — installs the result as
`/data/apps/tally2/tally2.elf`, and the shell then runs it as a genuine
pipeline stage: `ls / | tally2 | get rows` agrees with the host-built `tally`
on the same input (an A/B the test performs each run). Debugging this oracle
also flushed out and fixed a real shell bug: `eval_extern`'s streaming pump
closed the child's stdin write end with `embk_close_handle()` on an *fd*
number, so no consumer ever saw EOF — every `x | tally` pipeline had been
wedging since the pump rework landed.

**The ABI directory is the search path.** Since the §6.1 layout migration the
toolchain inputs live in the sealed ABI: `/system/abi/{crt0.o, syscalls.o,
libc.a}` — `libc.a` (6.6 MB) is the *same archive* the cross-build uses, so a
program built **on** EmbLink and one built **for** it are the same program.
tcc's search paths are baked in at configure time
(`tools/tcc/build-tcc-emblink.sh`): `-lXXX` resolves in `/system/abi`, and
`#include <...>` resolves in `/system/abi/include` (newlib's full header tree —
declaring the libc is part of the ABI contract, so the headers live beside the
objects that implement it) then `/data/apps/tcc/include` (tcc's own compiler
headers — `stddef.h`, `stdarg.h`, … — which belong to the compiler, so they
live with the app). Both trees are packed by mkfs from `EMBK_NEWLIB_INC` /
`EMBK_TCC_INC` (Makefile-derived, like `EMBK_NEWLIB_LIBC`).

**`-static -nostdlib` are load-bearing, not tidiness.** `-nostdlib` drops tcc's
host `crt1/crti/crtn` (wrong here — our entry contract is crt0's
`_start(argc, argv, envp)`) and its implicit `-ltcc1`. `-static` matters more:
without it tcc emits `PT_INTERP=/lib64/ld-linux-x86-64.so.2`, asking for an ELF
interpreter to be exec'd — a Linux contract this OS does not implement and will
not. `e_phnum` (2 static, 5 dynamic) is printed by the test as the direct
readout of that.

### Our four TCC patches, and why a pristine tcc is dangerous

`tools/tcc/0001-static-link-plt32-is-pc32.patch` — in a static link,
`R_X86_64_PLT32` must become a plain `PC32` call.

A PLT exists so a shared object can **preempt** a call. A static link has no
shared objects and no runtime resolver, so every PLT slot it mints is
unbindable — and upstream's `relocate_plt()` runs only under `if (dynamic)`, so
the stubs keep a placeholder displacement and `jmp *0x18(%rip)` reads its target
from **inside `.plt`**, executing neighbouring stub bytes as an address. tcc took
this path for **every ordinary call** — 407 of them (`main`, `malloc`, `memcpy`,
`exit`).

**It does not fail at build time.** tcc exits 0, the ELF is valid, and the
program dies at a wild jump. That silence is why the patch is applied by
`tools/tcc/build-tcc-emblink.sh` and not left to a README instruction.

`tools/tcc/0002-x86_64-gcc-type-macros.patch` — tcc 0.9.27 predefines almost
none of GCC's type/limit macros (`__INT64_TYPE__`, `__INTPTR_TYPE__`, …), and
newlib's headers select every fixed-width type from them **unconditionally**:
`sys/_intsup.h` redefines the type keywords as small integers and evaluates
`__INTPTR_TYPE__` *arithmetically*, then `#error`s without it. So a pristine
tcc compiles header-free toys and nothing else — the very first real
`#include <stdint.h>` dies. The patch defines the 67 missing macros for x86_64
LP64, values taken verbatim from `x86_64-elf-gcc -dM -E` on the same target
(nothing hand-derived). Proven on the host before it reached the OS: a native
tcc with this patch compiles a five-header program against the real newlib
tree, and the object links against `crt0.o + syscalls.o + libc.a` with zero
undefined symbols — **and no `libgcc`** (measured: `libc.a` references no
`__*ti3`-class intrinsics at all, so `-lgcc` is genuinely unnecessary on this
target, not skipped on hope).

`tools/tcc/0003-static-link-relocate-got.patch` — a **static** link never
relocated its own GOT. Upstream walks the reloc sections but skips `s1->got`
unless a dynamic loader will be present, which is right on Linux and wrong
here: nothing else is going to fill those slots. So every GOTPCREL data access
read a zero — and newlib reaches `stderr` and `errno` through `_impure_ptr`,
which is exactly such an access. The program dies dereferencing NULL the first
time it touches a stream or an error code.

Note what found it: **EmbBuild rebuilding EmbBuild**. The toy tests and even
`test tcc real` had passed; it took a program large enough to touch
`_impure_ptr` on a path that mattered. That is the argument for the on-OS
rebuild loop as a *test*, not just a feature.

`tools/tcc/0004-dynexe-pull-shared-lib-libc-imports.patch` — a shared object's
**undefined** symbols must be pulled into the dynamic executable that loads it.
There is no runtime `libc.so` on this OS (newlib is static and non-PIC), so
`libembk.so`'s imports — `vsnprintf`, `cosf`, `memcpy`, … — can only be
satisfied from the app's own re-exported newlib. GNU `ld` does this pulling
when the `.so` precedes `-lc` on the command line; tcc did not, and the link
succeeded anyway, producing an app missing precisely the symbols the toolkit
needed. The in-kernel loader then refused it at load time — the *right* failure,
but one boot cycle away from the cause.

### The GUI wall, and how it came down

The static shape above builds tools. Every **EmUI** app needs the other shape:
an `ET_EXEC` with a `PT_DYNAMIC`, bound to `/system/lib/libembk.so` by the
in-kernel loader (there is no `ld.so` — see `docs/architecture/`). Until
2026-07-23 the OS could not produce one, and that was the last thing its own
toolchain could not do.

`test tcc dyn` is the claim, and it is the same *kind* of claim as `exit=42` —
the machine, not the artifact:

```
[tcc dyn] compile exit=0
[tcc dyn] link exit=0
[tcc dyn] clockw2.elf: 103704 bytes, ET_EXEC=1 phnum=5 (dynamic>2)
ELF dynlink: /system/lib/libembk.so linked
[tcc dyn] spawn tcc-built widget -> pid=8
compositor: shared window 3 created (190x108, 21 pages) for pid 8
Clock: widget up / Clock: widget first frame
[tcc dyn] widget alive after run: YES (loaded + running)
```

It does not merely load: it takes a compositor window and **draws a frame**.
`phnum=5` is the direct readout that the dynamic shape took (2 = static).

Three things had to be true at once, and only one of them was tcc's fault:

1. **`user/lib/emlink_dynstubs.s`** — the tcc-world equivalent of `newlib.ld`'s
   `PROVIDE()`s. crt0 brackets its optional features with weak symbols
   (`__ctors_start/end`, `__tls_image/filesz/memsz/align`); GNU `ld` binds an
   unresolved weak reference to 0, but tcc turns it into a **dynamic import**
   the loader cannot satisfy. Defining the six as **weak absolute 0** is the
   correct geometry for an app with no static ctors and no `__thread` — which
   is exactly what tcc can build. Weak, so a real definition still wins.
2. **Patch 0004** (above) — the `.so`'s libc imports.
3. **A kernel bug of our own**, which is the interesting one. The loader's
   `resolve_sym()` returned the symbol's *value* and used `0` to mean "not
   found" — so a symbol whose legitimate value **is** zero could not be told
   apart from a missing one. The six bracket symbols are `SHN_ABS 0` by
   construction. They were found, resolved correctly, and then reported
   `UNRESOLVED` by their own caller. Splitting found-ness from value (return
   `bool`, value through an out-param) fixed it, and reading that function with
   the absolute case in mind surfaced two more real defects: `SHN_ABS` was
   getting the load bias added (invisible for the app at bias 0; a wild pointer
   inside `libembk.so` at `DYLIB_VA_BASE`), and a weak reference to a genuinely
   absent symbol failed the load instead of binding to 0 — which is what weak
   *means*.

**And then the build tool learned the shape.** `test tcc dyn` drives tcc
directly, with a hand-written argv — which proves the *toolchain* can do it and
nothing more. `test embbuild gui` closes the remaining distance: EmbBuild
builds the same widget from `/data/src/ui/build.ebm`, and the dynamic link line
survives being written down as a manifest stanza (BUILD.md §6). The staged ELF
comes out `ET_EXEC phnum=5`, the kernel binds it, it renders, the rerun reports
`0 ran, 3 up_to_date`, and the adopted binary runs. Note that EmbBuild's
grammar needed **no extension** for a link line this different from the static
ones — the evidence that "recipes are argv arrays" was the right primitive.

**The method is worth more than the fix.** The failing artifact was reproduced
**on the host** by building a host-native tcc from the same patched source
(`cp -r` the tree, configure without `--cross-prefix`) and running the identical
compile and link. It produced a **byte-identical 103704-byte** binary, and one
line of `readelf --dyn-syms` ended the investigation:

```
5: 0000000000000000  0 NOTYPE  WEAK  DEFAULT  ABS __tls_memsz
   R_X86_64_GLOB_DAT  __tls_memsz + 0
```

Seconds, against a multi-minute boot-and-guess loop. The reason this was not
tried sooner is that the on-image `tcc.elf` **cannot** run on the host — it is
cross-linked at `0x400000` and segfaults — so "we have no host tcc" looked true
and was not. Building a second, native one costs about a minute.

`build/libtcc1.o` is packed to `/system/abi` for the same reason: it holds the
float and 64-bit intrinsics tcc's codegen *emits* but gcc inlines, so they are
absent from x86_64 `libgcc`. (An earlier edition of this document listed "no
`libtcc1.a`" as a limit; it is cross-built here now.)

### Known limits (all real, none hidden)

- **No `__thread` in tcc-built programs.** TCC has its own linker and reads no
  GNU linker script, so `newlib.ld`'s `PT_TLS` is unavailable. crt0 already
  copes: its TLS geometry symbols are weak, so `__tls_memsz == 0` and TLS setup
  does nothing. Bounded and understood.
- **`tcc -run` is out** — it wants `dlopen`, which this OS genuinely lacks.
- **Codegen is tcc's**: no optimiser worth the name. Correct, not fast. Nothing
  here has ever needed it to be fast.

## CPython: one patch, and a config.site that is real knowledge

`tools/cpython/0001-newlib-timezone-and-clockid.patch` — newlib spells the
timezone globals the way MSVC does (`_timezone`/`_daylight`/`_tzname`, no plain
`timezone`), and its `clockid_t` is long-sized, tripping a `Py_BUILD_ASSERT`.
Both gated on `__NEWLIB__`. This one fails loudly at compile time.

`tools/cpython/emblink-config.site` is the other half and matters more than it
looks: **EmbLink binaries cannot run on the build host**, so autoconf can never
execute a probe. Every such answer is pre-cached there, and each states what is
actually true of this OS. Never flip one to "yes" to get past configure — a
false yes compiles a call to something that does not exist, and resurfaces as a
link error or, worse, as silent wrong behaviour at runtime.

The stdlib ships as a **stored** (uncompressed) zip via frozen `zipimport`.
`MODULE_BUILDTYPE=static` builds every extension module into the binary, because
there is no `dlopen`.

## git: zero patches

The whole trick is `make uname_S=EmbLink` **on the command line**, so
`config.mak.uname`'s Linux block never applies and git starts from its most
conservative defaults; `config.mak` then states exactly what EmbLink has. Its
tree is genuinely pristine — verifiable with `git status` in it.

git needs a cross-built **zlib**: its object store *is* deflate + crc32, and
`git init` cannot write its first object without one.

## The staleness trap that outlives all of them

**`python.elf`, `git.elf` and `tcc.elf` statically link `build/syscalls.o`** via
a raw path in their own `LDFLAGS` — which their build systems treat as a plain
file, not a prerequisite. So CPython's `make python` cheerfully says *"up to
date"* however much our libc changed. `python.elf` once sat **15 hours stale**,
passing `test python` against a frozen copy of an old libc: green, and true of a
binary nobody builds any more.

Our Makefile now tracks it — those `.elf` rules take `crt0.o`/`syscalls.o` as
real prerequisites and force the external relink only when the libc is genuinely
newer. `touch user/lib/syscalls.c && make` prints `RELINK python` / `RELINK git`;
a second `make` is silent. The relink is not a formality: it immediately caught
our `dup2` colliding with CPython's own (fixed by making the libc's **weak** — a
libc stub must yield to an app shipping its own).

When in doubt, check mkfs's `src fingerprint` line for the packed binary.

## Ports considered and deliberately deferred (with the reason on record)

- **GNU make** — deferred, not because recipes need `/bin/sh` here (audited:
  every userland recipe is one argv = one `spawn()`), but because the *only*
  thing a make port buys is compatibility with foreign trees — and the host
  Makefile can't run on-OS regardless (cross-gcc paths, GNU-make-4.3 features,
  host Python for mkfs). Rebuild-self goes to a **native structured build
  tool** (targets as typed records, recipes as argv arrays, staleness by
  content hash — the RTC's one-second mtimes against millisecond compiles make
  timestamp staleness structurally false-fresh here). make arrives later as
  opt-in compat when a foreign tree (rebuilding git/CPython on-OS) demands it,
  alongside the sh/sed dragnet that story implies.
- **clang / clangd** — unlike GCC, *not* structurally impossible: modern clang
  runs cc1 in-process (`-fintegrated-cc1`) and lld links as a library, and
  clangd's core is in-process too. What defers them is cost vs. value: an
  LLVM cross-build against newlib with no mmap and a single-threaded
  libstdc++ is a CPython-port-times-twenty effort, for C capability TCC
  already delivers — and clangd additionally needs an LSP client that doesn't
  exist on-OS. The one future thing that would justify the climb is **C++
  compiled on-OS** (TCC can never do that). The near-term path to the *felt*
  value (diagnostics while editing on-OS) is the V7 TextEditor spawning
  `tcc -c` with piped stderr and parsing `file:line:` — every piece already
  shipped and proven.
