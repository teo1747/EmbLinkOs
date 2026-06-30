/* user/init.c — the first userland program. Freestanding: no kernel headers,
 * its own syscall wrappers. Linked at 0x400000 (see user.ld), flattened to a
 * raw binary. Every address it references is a 0x400xxx address, so it MUST be
 * loaded at 0x400000. */

static long syscall3(long n, long a1, long a2, long a3)
{
    long ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(n), "D"(a1), "S"(a2), "d"(a3)
        : "rcx", "r11", "memory"
    );
    return ret;
}

#define SYS_write 1
#define SYS_exit  2

static long write(int fd, const char *buf, unsigned long len)
{
    return syscall3(SYS_write, fd, (long)buf, (long)len);
}

__attribute__((noreturn))
static void exit(int code)
{
    syscall3(SYS_exit, code, 0, 0);
    for (;;) { }   /* exit never returns; satisfy noreturn */
}

/* strlen, because we have no libc yet. */
static unsigned long ustrlen(const char *s)
{
    unsigned long n = 0;
    while (s[n]) n++;
    return n;
}

/* The entry point. The linker script makes THIS the first thing in the binary,
 * so the program's entry == its load base == 0x400000. */
__attribute__((noreturn, section(".text.start")))
void _start(void)
{
    const char *msg = "Hello from a real user program loaded at 0x400000!\n";
    write(1, msg, ustrlen(msg));
    exit(0);
}