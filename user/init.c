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

#define SYS_write         1
#define SYS_exit          2
#define SYS_thread_create 14
#define SYS_thread_join   15
#define SYS_thread_exit   16

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

/* thread_create(entry, arg) -> tid, or < 0 on failure. `entry` runs as an
 * ADDITIONAL thread of THIS process (Phase 5's ring-3 thread syscalls),
 * sharing this exact address space -- ordinary function-pointer semantics
 * work here (unlike usermode.c's kernel-side user_stub), since this whole
 * ELF is loaded and always runs at its normal linked address. `arg` arrives
 * in the new thread's RDI, i.e. as `entry`'s own first C parameter -- no
 * special handling needed, the kernel's process_trampoline sets it up
 * before the new thread's very first instruction. */
static long thread_create(void (*entry)(long arg), long arg)
{
    return syscall3(SYS_thread_create, (long)entry, arg, 0);
}

/* Block until thread `tid` (of THIS process) exits; returns its exit code. */
static long thread_join(long tid)
{
    return syscall3(SYS_thread_join, tid, 0, 0);
}

__attribute__((noreturn))
static void thread_exit(int code)
{
    syscall3(SYS_thread_exit, code, 0, 0);
    for (;;) { }   /* thread_exit never returns; satisfy noreturn */
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


volatile int  counter = 5;          /* volatile -> real .data store/load */
volatile char scratch[64];          /* volatile -> real .bss, not elided  */
volatile int  thread_ran = 0;       /* set only by second_thread_entry -- proves
                                      * _start can observe writes the OTHER
                                      * thread made, i.e. genuinely shared
                                      * .data, not two separate copies */

/* Runs as an ADDITIONAL thread of this SAME process (thread_create() below),
 * on its own dedicated user stack but the exact same address space as
 * _start. Writes to `counter`/`thread_ran` here must be visible to _start
 * after thread_join() returns below, or the two "threads" don't actually
 * share memory -- the whole point of a thread instead of a second process. */
static void second_thread_entry(long arg)
{
    write(1, "Hello from thread 2!\n", 22);
    thread_ran = 1;
    counter += (int)arg;             /* real write to the SAME .data as _start's */
    thread_exit(99);
}

void _start(void)
{
    write(1, "Hello from ELF!\n", 16);

    counter++;                       /* real write to .data                */

    /* touch bss so it can't be elided, and prove it was ZEROED: sum it —
     * must be 0 if the loader zeroed the p_memsz - p_filesz tail */
    int sum = 0;
    for (int i = 0; i < 64; i++) sum += scratch[i];

    /* Phase 5: spawn a second thread under THIS process, wait for it, and
     * check that its writes (thread_ran, counter += 10) are visible here --
     * proof this is a real shared-address-space thread, not a second
     * process that merely happens to run alongside this one. */
    long tid = thread_create(second_thread_entry, 10);
    int thread_ok = 0;
    if (tid >= 0) {
        long code = thread_join(tid);
        thread_ok = (code == 99) && thread_ran && (counter == 16);
    }

    /* expect 5 (init) + 1 (_start) + 10 (thread) + 0 (bss sum) = 16 on
     * success; -1 if thread creation/join/the shared-memory check failed. */
    exit(thread_ok ? (counter + sum) : -1);
}
