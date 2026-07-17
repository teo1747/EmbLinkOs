# Ports: C++, CPython, git, and a C compiler on the OS

EmbLinkOS runs four substantial pieces of third-party software. This doc says
what each one actually does here, what it honestly cannot do, and why the OS
grew the way it did to host them. For *how to build* them, see
[BUILD_SETUP.md](BUILD_SETUP.md#optional-ports-c-cpython-git-tcc); this doc is
the *what and why*.

None of them is a fork. Between them they needed **one** patch each to CPython
and TCC, and **zero** to git.

| Port | Status | Proof it works | Needs |
|---|---|---|---|
| C++ / libstdc++ | runs | `test cxx` → 13/13 | — |
| CPython 3.14 | runs | `test python` → "hello from CPython on EmbLink" | `-cpu max` |
| git 2.49 | runs | `test git repo` → a real root commit | `-cpu max` |
| **TCC 0.9.27** | **runs, and compiles** | **`test tcc link` → `exit=42`** | — |

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
- **TCC** → `libc.a` on the image, and proof the flat root is a real search path.
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

**The flat root is the search path.** mkfs packs no directories, so `crt0.o`,
`syscalls.o` and `libc.a` (6.6 MB — the largest object on the image) live at
`/`, and tcc is told `-L/`. No `/lib`, no `/include`, no directory support
needed. `libc.a` is the *same archive* the cross-build uses, so a program built
**on** EmbLink and one built **for** it are the same program.

**`-static -nostdlib` are load-bearing, not tidiness.** `-nostdlib` drops tcc's
host `crt1/crti/crtn` (wrong here — our entry contract is crt0's
`_start(argc, argv, envp)`) and its implicit `-ltcc1`. `-static` matters more:
without it tcc emits `PT_INTERP=/lib64/ld-linux-x86-64.so.2`, asking for an ELF
interpreter to be exec'd — a Linux contract this OS does not implement and will
not. `e_phnum` (2 static, 5 dynamic) is printed by the test as the direct
readout of that.

### Our one TCC patch, and why a pristine tcc is dangerous

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

### Known limits (all real, none hidden)

- **No `__thread` in tcc-built programs.** TCC has its own linker and reads no
  GNU linker script, so `newlib.ld`'s `PT_TLS` is unavailable. crt0 already
  copes: its TLS geometry symbols are weak, so `__tls_memsz == 0` and TLS setup
  does nothing. Bounded and understood.
- **No `libtcc1.a`** (`__divdi3` and friends). Its Makefile builds it by *running*
  the just-built tcc, which the host cannot execute. `-nostdlib` skips it; a
  program needing those helpers would fail at link, loudly.
- **`tcc -run` is out** — it wants `dlopen`, which this OS genuinely lacks.

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
