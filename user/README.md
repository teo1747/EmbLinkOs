# EmbLink userland: the SDK + newlib port

Two complementary layers let a program use the full C library *and* the
OS-native primitives POSIX can't express.

## Layers

| File | What it is | Who uses it |
|------|-----------|-------------|
| `embk_syscall.h` | Raw `int 0x80` ABI: syscall numbers + register wrappers (`embk_syscall0..5`) + the `[-4095,-1]` error-range helper. Hand-synced to `kernel/cpu/syscall.c`. | Everything below. |
| `embk.h` | **The EmbLink SDK.** Header-only, typed native primitives: `embk_spawn`, `embk_thread_create/join/exit`, `embk_wait/kill`, `embk_yield`, `embk_readdir`, `embk_stat`, file-actions. Raw kernel return convention (`ret < 0` is `-EMBK_*`, no errno). | Freestanding **and** newlib programs. |
| `crt0.c` | newlib entry stub: aligns the stack for a raw ELF entry, calls `main`, `exit`s. | newlib programs only. |
| `syscalls.c` | **The newlib retargeting layer.** POSIX stubs (`read`, `write`, `open`, `sbrk`, `fstat`, `gettimeofday`, …) that back newlib's libc; set `errno`, return `-1` on failure. | newlib programs only. |

## Two ways to build a program

**Freestanding** (no libc — smallest, e.g. `init.c`): write your own `_start`,
`#include "embk.h"`, link just your `.o` with `user.ld`. You get spawn,
threads, sbrk, VFS — no printf/malloc.

**newlib** (full libc, e.g. `hello.c`): `#include <stdio.h>` etc. for libc and
`#include "embk.h"` for native primitives. Link `crt0.o + syscalls.o +
yourprog.o -lc -lgcc` with `newlib.ld`. You get printf/malloc/string/time
**and** spawn/threads. This is the recommended path for real programs.

`make user/hello.elf` builds the newlib demo; `make user/init.elf` builds the
freestanding self-test.

## printf format support (fixed via a newlib rebuild)

The **stock** newlib in the cross toolchain (`/usr/local/cross`, newlib 4.6.0)
was configured with `_WANT_IO_C99_FORMATS` and `_WANT_IO_LONG_LONG` **off**, so
`%z`/`%j`/`%t` and `%ll` were compiled out of its `libc.a` — `%zu` printed a
literal `"zu"`. That config is baked into `libc.a` at newlib build time and
can't be toggled from a user program.

Fixed by rebuilding newlib **with** those options into a user-owned prefix:

```
../newlib-4.6.0.../configure --target=x86_64-elf \
    --prefix=/home/motsou/cross/newlib-c99 \
    --disable-newlib-supplied-syscalls --disable-multilib --disable-nls \
    --enable-newlib-io-c99-formats --enable-newlib-io-long-long
make && make install
```

The Makefile's `NEWLIB_PREFIX` points the newlib compile/link at that prefix
(`-isystem …/include`, `-L …/lib` — searched before the toolchain's default,
so the fuller `libc.a` wins). `%z`, `%ll`, etc. now work. To fall back to the
stock toolchain newlib, build with `NEWLIB_PREFIX=` (empty).
