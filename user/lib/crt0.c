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
 * touches rdi/rsi, so argc/argv arrive at start_c() exactly as
 * process_trampoline left them. start_c()/exit() never return in practice;
 * the trailing hang loop is only a defensive backstop. */
__asm__(
    ".global _start\n"
    "_start:\n"
    "    and $-16, %rsp\n"
    "    call start_c\n"
    "1:  jmp 1b\n"
);

__attribute__((noinline, used))
static void start_c(int argc, char **argv)
{
    exit(main(argc, argv, (char **)0));
}
