/* user/crt0.c — EmbLink OS's newlib crt0. No _start.S, no stack-popping:
 * process_trampoline already delivers argc/argv in rdi/rsi, exactly where a
 * normal C function expects its first two parameters. The only asm needed
 * is the three-instruction _start stub below -- not for argument delivery
 * (rdi/rsi pass through untouched), but because a raw ELF entry point and
 * an ordinary CALLed C function follow DIFFERENT stack-alignment
 * conventions that a plain C function can't reconcile for itself (see
 * _start's comment below).
 *
 * Deliberately missing, not overlooked: no envp (nothing builds an
 * environment yet -- passed as NULL), no __libc_init_array() call (skips
 * C++ static constructors / GCC constructor attributes -- fine for a
 * pure-C toolchain; revisit only if this ever needs to build C++). */

/* The one include: crt0 otherwise declares its externs by hand (no libc headers
 * this early), but setup_tls() has to reach the kernel to install %fs and
 * hand-rolling the syscall stub would just duplicate this header. It is
 * freestanding -- inline asm and constants only. */
#include "embk_syscall.h"

extern int main(int argc, char **argv, char **envp);
extern void exit(int code);   /* newlib's real exit() -- NOT user/init.c's
                                * hand-rolled one. This one runs atexit()
                                * handlers and flushes stdio before calling
                                * newlib's _exit(), which is OUR syscalls.c
                                * stub (the one wrapping SYS_exit). */

/* The REAL entry point: three instructions, not a C function, on purpose.
 * process_trampoline() delivers control via a raw `iretq` (no `call`), with
 * RSP set to a 16-byte-aligned child_user_rsp (process.c's `off &= ~0xFULL`)
 * -- correctly following the x86-64 SysV ABI's PROCESS-entry convention
 * (RSP === 0 mod 16, no synthetic return address on the stack). A plain C
 * function compiled as `_start` can't know that: GCC always assumes the
 * ORDINARY called-function convention instead (RSP === 8 mod 16, as if a
 * `call` just pushed an 8-byte return address) and arranges its own
 * internal `call`s to land on 0-mod-16 relative to THAT assumed baseline.
 * Since the real baseline here is already 0-mod-16, a plain-C `_start`'s
 * `call main` would actually execute misaligned by 8 bytes. Silent today
 * only because this whole toolchain builds with -mno-sse/-mno-sse2 (no
 * aligned SIMD load/store ever gets emitted to fault on it) -- would become
 * a real, hard-to-diagnose crash the moment real newlib code (compiled
 * normally, using SSE-optimized memcpy/memset/etc.) gets linked in.
 *
 * `and $-16, %rsp` re-establishes 16-byte alignment (a no-op today given
 * child_user_rsp is already aligned, but this is the same defensive
 * realignment every real crt0 does, and it's what a raw entry point is
 * responsible for providing for itself). `call start_c` -- not `jmp` --
 * is what then correctly turns that 0-mod-16 baseline into the 8-mod-16
 * a normally-compiled C function expects at ITS OWN entry (the `call`'s
 * pushed return address IS that missing 8 bytes). Neither instruction
 * touches rdi/rsi/rdx, so argc/argv/envp arrive at start_c() exactly as
 * process_trampoline left them. start_c()/exit() never return in practice;
 * the trailing hang loop is only a defensive backstop. */
__asm__(
    ".global _start\n"
    "_start:\n"
    "    and $-16, %rsp\n"
    "    call start_c\n"
    "1:  jmp 1b\n"
);

/* ---- the crt-stuff a -nostartfiles link doesn't get --------------------
 * `__dso_handle` normally comes from crtbegin.o. We link -nostartfiles (crt0
 * provides _start), so nothing defines it -- and the FIRST C++ program with a
 * global destructor fails to link: g++ emits
 * `__cxa_atexit(dtor, obj, &__dso_handle)`, where the handle identifies WHICH
 * module owns the destructor so a dlclose() could run just that module's.
 *
 * Its own address is the conventional value: it only has to be a unique,
 * stable token per module, never dereferenced. There is exactly one module
 * here, so uniqueness is free -- but define it we must, or C++ objects with
 * static-storage destructors simply cannot link. */
void *__dso_handle = &__dso_handle;

/* ---- static initializers (.init_array) ---------------------------------
 * Emitted by the linker script (newlib.ld) with these bracket symbols, but
 * NOTHING RAN THEM until now -- the array was laid down and ignored.
 *
 * Each entry is a function the compiler wants called BEFORE main:
 *   - C++ global/static constructors (`static Foo g_foo;`)
 *   - GCC's __attribute__((constructor)) in plain C
 * Skipping them is why crt0 was "pure-C only": a C++ program would enter
 * main with every global still raw zeroes, then crash or silently misbehave
 * the moment it touched one. This is the crt0 half of C++ support.
 *
 * Hand-rolled rather than newlib's __libc_init_array() because that also
 * runs _init/.init (the legacy crtbegin/crtend path) which this bare
 * -nostartfiles link has no crtX objects for. Walking the array IS the
 * modern contract; order is the linker's (SORT'ed by priority).
 *
 * TWO schemes are walked, because GCC chooses between them at ITS OWN
 * configure time (--enable-initfini-array): the stock bare-metal C cross
 * compiler (built --without-headers) emits the LEGACY .ctors, while a
 * newlib-aware/C++ build emits the modern .init_array. Walking only one
 * silently does nothing for objects built by the other -- exactly the trap
 * this hit first time round: the constructor sat unrun in an uncollected
 * .ctors while the .init_array brackets came out empty (start == end). Run
 * both; whichever is empty costs a single compare.
 *
 * .fini_array (destructors / atexit-at-image-scope) is deliberately NOT run
 * here: exit() is what owns that, and nothing needs it yet. Named, not
 * forgotten. */
/* WEAK on purpose: this ONE crt0.o is linked two different ways. Static
 * programs (shell/sysinfo/tally/hello) use `-T user/lib/newlib.ld`, which
 * defines all four brackets. The dynamically-linked EmUI apps
 * (NEWLIB_DYN_LDFLAGS -- no -T, ld's DEFAULT script) get
 * __init_array_start/end from that script but NOT __ctors_start/end, so a
 * strong reference is an instant "undefined reference to __ctors_end" and
 * every EmUI app stops linking. Weak means an absent bracket resolves to 0
 * and the walk is simply skipped -- crt0 stays correct under any linker
 * script, present or future. */
extern void (*__init_array_start[])(void) __attribute__((weak));
extern void (*__init_array_end[])(void) __attribute__((weak));
extern void (*__ctors_start[])(void) __attribute__((weak));
extern void (*__ctors_end[])(void) __attribute__((weak));

/* ------------------------------------------------------------------ */
/* Thread-local storage                                                */
/* ------------------------------------------------------------------ */

/* Geometry of the TLS template, handed over by user/lib/newlib.ld. These are
 * ABSOLUTE linker symbols: their VALUE is the number, so we take the ADDRESS
 * and cast -- reading them as variables would dereference address 0x10-ish and
 * fault.
 *
 * WEAK for the same reason as the constructor brackets above, and it is not
 * hypothetical: crt0.o is linked TWO ways. Static programs use -T newlib.ld,
 * which defines these; the dynamic EmUI apps use NEWLIB_DYN_LDFLAGS (no -T,
 * ld's DEFAULT script), which does NOT. A strong reference would break every
 * EmUI app's link instantly. Weak ⇒ absent ⇒ 0 ⇒ __tls_memsz == 0 ⇒ the setup
 * below does nothing, which is exactly right for a program with no TLS.
 * KNOWN GAP: that also means dynamically-linked EmUI apps get no TLS at all --
 * fine while nothing there uses __thread, but a `__thread` variable in one
 * would fault on %fs. Fix by teaching the dynamic link the same symbols. */
extern char __tls_image[]  __attribute__((weak));
extern char __tls_filesz[] __attribute__((weak));
extern char __tls_memsz[]  __attribute__((weak));
extern char __tls_align[]  __attribute__((weak));

extern void *malloc(unsigned long size);
extern void *memcpy(void *dst, const void *src, unsigned long n);
extern void *memset(void *dst, int c, unsigned long n);

/* Room reserved at the thread pointer for the Thread Control Block. Only the
 * self-pointer at offset 0 is architecturally required (`mov %fs:0x0,%reg` is
 * how every TLS access starts), but the psABI's TCB conventionally holds more
 * -- notably a DTV pointer at +8 and the stack-protector canary at +0x28. We
 * reserve and ZERO 64 bytes so anything that pokes at those reads a defined
 * zero rather than heap garbage. */
#define TCB_SIZE 64

/* Build this thread's static TLS block and point %fs at it.
 *
 * x86-64 uses TLS VARIANT II: the block sits BELOW the thread pointer, and the
 * linker resolves a variable at TLS offset `o` to  TP - ALIGN(memsz, align) + o.
 * That formula is why we must round `memsz` up with the linker's OWN align value
 * (__tls_align) -- pick a different one and every TLS variable silently lands at
 * the wrong address, which is far worse than a fault.
 *
 * Layout built here:
 *
 *     [ .tdata copy | zeroed .tbss ][ TCB ]
 *     ^block                        ^TP == fs_base, *(void**)TP == TP
 *
 * Called before the constructors: a C++ ctor may touch a __thread variable.
 * malloc() is safe this early -- newlib is built --enable-threads=single, so its
 * allocator uses _impure_ptr, not TLS; it would be circular otherwise. */
static void setup_tls(void)
{
    unsigned long memsz  = (unsigned long)(unsigned long *)__tls_memsz;
    unsigned long filesz = (unsigned long)(unsigned long *)__tls_filesz;
    unsigned long align  = (unsigned long)(unsigned long *)__tls_align;

    if (memsz == 0) {
        return;             /* no PT_TLS: nothing to set up, %fs stays unused */
    }
    if (align == 0) {
        align = 8;
    }

    unsigned long tls_size = (memsz + align - 1) & ~(align - 1);

    /* + align of slack so TP can be rounded UP to the linker's alignment while
     * the block below it still fits inside the allocation. */
    char *base = (char *)malloc(tls_size + TCB_SIZE + align);
    if (!base) {
        return;             /* nothing sane to do this early; a TLS read will
                             * fault loudly rather than read someone else's data */
    }

    unsigned long tp = ((unsigned long)base + tls_size + align - 1) & ~(align - 1);
    char *block = (char *)(tp - tls_size);

    if (filesz) {
        memcpy(block, __tls_image, filesz);        /* the .tdata initialisers */
    }
    memset(block + filesz, 0, tls_size - filesz);  /* .tbss + alignment tail */
    memset((void *)tp, 0, TCB_SIZE);

    *(unsigned long *)tp = tp;   /* the self-pointer `mov %fs:0x0,%reg` reads */

    /* Only the kernel can write IA32_FS_BASE: CR4.FSGSBASE is off, so WRFSBASE
     * would #UD here. Ignore the return -- if it fails the first TLS access
     * faults, which is a better signal than anything we could print now. */
    embk_syscall1(EMBK_SYS_set_fs_base, (int64_t)tp);
}

static void run_init_array(void)
{
    if (!__init_array_start || !__init_array_end) {
        return;                 /* this link has no such section */
    }
    /* modern: forward, linker-sorted by priority */
    for (void (**fn)(void) = __init_array_start; fn != __init_array_end; fn++) {
        if (*fn) {
            (*fn)();
        }
    }
}

static void run_ctors(void)
{
    if (!__ctors_start || !__ctors_end) {
        return;                 /* e.g. the default script: no brackets */
    }
    /* legacy .ctors: BACKWARD -- that is this format's contract (crtbegin's
     * __do_global_ctors_aux walks from the end down). No crtbegin/crtend
     * means no -1/0 sentinels, but skip them defensively in case a linked-in
     * object ever brings its own. */
    for (void (**fn)(void) = __ctors_end; fn-- != __ctors_start; ) {
        if (*fn && *fn != (void (*)(void))-1) {
            (*fn)();
        }
    }
}

/* Defined in syscalls.c. `environ` is what getenv()/setenv() walk; the kernel
 * hands us the vector in RDX and this is where the two meet. */
extern char **environ;
extern char *embk_empty_env[1];

/* syscalls.c: seeds the working directory from PWD, if the parent named one.
 * Must run AFTER environ is installed and BEFORE anything resolves a relative
 * path -- a ctor that opens a data file is exactly that. */
extern void embk_cwd_init_from_env(void);

__attribute__((noinline, used))
static void start_c(int argc, char **argv, char **envp)
{
    /* Publish the environment BEFORE ctors and main: a C++ static initialiser
     * or an __init_array entry may legitimately call getenv().
     *
     * envp==NULL means the parent gave us NO environment (the EmbLink default --
     * nothing is inherited unless named; see kernel spawn.h). Point `environ` at
     * an empty vector rather than leaving it NULL: getenv() must be able to walk
     * it and answer "unset", and every caller expects environ to be
     * dereferenceable. "No environment" and "an environment with nothing in it"
     * are indistinguishable to a reader, and only one of them is safe. */
    environ = envp ? envp : embk_empty_env;

    /* The working directory a parent HANDED us (PWD), or "/" if it named none.
     * Nothing is inherited on EmbLink -- see syscalls.c's g_cwd. Ordered after
     * environ (it reads it) and before ctors (one may open a relative path). */
    embk_cwd_init_from_env();

    setup_tls();                            /* %fs BEFORE any ctor: one may
                                             * touch a __thread variable, and a
                                             * TLS read with %fs unset faults at
                                             * address 0 */
    run_init_array();                       /* constructors BEFORE main ... */
    run_ctors();                            /* ... in either scheme */
    exit(main(argc, argv, environ));
}
