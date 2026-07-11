/* user/init.c — the first userland program and the EmbLink native-primitive
 * self-test. Freestanding: its own _start (no crt0), no libc. Built entirely
 * on the EmbLink SDK (user/embk.h) rather than hand-rolled syscalls, so it
 * doubles as the SDK's own dogfooding. Linked at 0x400000 (user/user.ld).
 *
 * TWO ROLES, selected by argc (this is load-bearing -- see _start):
 *   - Top level (argc == 1, e.g. `run /init.elf` or `test ring3 threads`):
 *     run the full native-primitive suite (threads, spawn, sbrk) and exit
 *     16 iff everything passed. 16 is the success sentinel the kernel's
 *     `test ring3 threads` selftest asserts on.
 *   - Spawned child (argc >= 2): the minimal behavior the parent's
 *     spawn_test() checks -- echo argv[1] to fd 3 (a file the parent
 *     redirected in via a spawn file-action) and exit argc. MUST exit here
 *     and never fall through into the suite, or it would recursively spawn
 *     itself forever. */

#include "embk.h"

/* --- shared state the thread test proves is genuinely shared ------------ */
volatile int  counter    = 5;   /* .data: mutated by the second thread below */
volatile int  thread_ran = 0;   /* set ONLY by second_thread_entry -- proof
                                  * _start observes the other thread's writes */

/* Runs as an ADDITIONAL thread of this same process (embk_thread_create),
 * on its own user stack but the exact same address space as _start. Its
 * writes to counter/thread_ran must be visible to _start after the join, or
 * the two "threads" don't actually share memory. `arg` arrives in RDI as
 * this function's first parameter. */
static void second_thread_entry(long arg)
{
    embk_puts(1, "Hello from thread 2!\n");
    thread_ran = 1;
    counter += (int)arg;
    embk_thread_exit(99);
}

/* thread_create + join, then verify the shared-memory side effects. Returns
 * 1 on pass, 0 on fail. */
static int thread_test(void)
{
    int start = counter;
    long tid = embk_thread_create(second_thread_entry, 11);
    if (tid < 0) { embk_puts(1, "thread_test: FAIL create\n"); return 0; }

    long code = embk_thread_join((int)tid);
    if (code != 99) { embk_puts(1, "thread_test: FAIL join code\n"); return 0; }

    if (!thread_ran || counter != start + 11) {
        embk_puts(1, "thread_test: FAIL shared memory not observed\n");
        return 0;
    }
    embk_puts(1, "thread_test: PASS\n");
    return 1;
}

/* Spawn a child (this same ELF, argc>=2 path) with argv AND a file-action
 * that pre-opens /spawn_test.tmp onto the child's fd 3; the child writes
 * argv[1] there. Then reopen the file and confirm the bytes round-tripped.
 * Proves argv delivery, spawn file-actions, wait, and the fd/VFS path all
 * work together. Returns 1 on pass, 0 on fail. */
static int spawn_test(void)
{
    char *argv[] = { "init", "hello-from-argv", (char *)0 };

    struct embk_spawn_file_action actions[1];
    actions[0].kind = EMBK_SPAWN_ACTION_OPEN;
    actions[0].target_fd = 3;
    { const char *p = "/spawn_test.tmp"; int i = 0;
      while (p[i]) { actions[0].path[i] = p[i]; i++; }
      actions[0].path[i] = '\0'; }
    actions[0].flags = EMBK_O_CREAT | EMBK_O_WRONLY | EMBK_O_TRUNC;
    actions[0].mode = 0644;

    long handle = embk_spawn("/init.elf", argv, actions, 1);
    if (handle < 0) { embk_puts(1, "spawn_test: FAIL spawn()\n"); return 0; }

    long exit_code = embk_wait((int)handle);
    if (exit_code != 2) { embk_puts(1, "spawn_test: FAIL wrong argc\n"); return 0; }

    long fd = embk_open("/spawn_test.tmp", EMBK_O_RDONLY, 0);
    if (fd < 0) { embk_puts(1, "spawn_test: FAIL reopen\n"); return 0; }
    char buf[64] = {0};
    long n = embk_read((int)fd, buf, sizeof(buf) - 1);
    embk_close((int)fd);

    if (n <= 0 || !embk_streq(buf, "hello-from-argv")) {
        embk_puts(1, "spawn_test: FAIL content mismatch\n");
        return 0;
    }
    embk_puts(1, "spawn_test: PASS\n");
    return 1;
}

#define EXPECTED_HEAP_BASE 0x0000600000000000L

/* Grow/shrink the heap via embk_sbrk and verify the mapping, persistence,
 * shrink-doesn't-unmap, and oversized-request-rejection behavior. Returns 1
 * on pass, 0 on fail. */
static int sbrk_test(void)
{
    long base = embk_sbrk(0);
    if (base != EXPECTED_HEAP_BASE) { embk_puts(1, "sbrk_test: FAIL initial break\n"); return 0; }

    long grow = 3 * 4096;   /* span three pages: exercises the mapping LOOP */
    long old_brk = embk_sbrk(grow);
    if (old_brk != base) { embk_puts(1, "sbrk_test: FAIL didn't return old break\n"); return 0; }

    /* Touch every page (a #PF here would be a loud, unambiguous fail). */
    volatile char *heap = (volatile char *)base;
    for (long i = 0; i < grow; i += 512) heap[i] = (char)(i & 0xFF);
    for (long i = 0; i < grow; i += 512) {
        if (heap[i] != (char)(i & 0xFF)) { embk_puts(1, "sbrk_test: FAIL data mismatch\n"); return 0; }
    }
    if (embk_sbrk(0) != base + grow) { embk_puts(1, "sbrk_test: FAIL break didn't persist\n"); return 0; }

    /* Shrink one page then regrow -- the regrown page must still hold its
     * bytes (heap_mapped_top never unmaps on shrink). */
    (void)embk_sbrk(-4096);
    if (embk_sbrk(0) != base + grow - 4096) { embk_puts(1, "sbrk_test: FAIL shrink\n"); return 0; }
    (void)embk_sbrk(4096);
    if (heap[grow - 4096] != (char)((grow - 4096) & 0xFF)) {
        embk_puts(1, "sbrk_test: FAIL regrow lost data\n"); return 0;
    }

    /* Absurd request must fail cleanly and not disturb the break. */
    long huge = embk_sbrk(0x0000000100000000L);   /* 4 GiB, past the 1 GiB cap */
    if (huge >= 0) { embk_puts(1, "sbrk_test: FAIL oversized didn't fail\n"); return 0; }
    if (embk_sbrk(0) != base + grow) { embk_puts(1, "sbrk_test: FAIL failed req corrupted break\n"); return 0; }

    embk_puts(1, "sbrk_test: PASS\n");
    return 1;
}

/* rdi=argc, rsi=argv -- delivered by the kernel's process_trampoline has_argv
 * path. No crt0: these C-ABI parameter registers ARE where the trampoline
 * put them. */
void _start(long argc, char **argv)
{
    /* Spawned-child role: do exactly what spawn_test() above checks, then
     * exit. Exiting HERE (not falling through) is what stops init from
     * recursively spawning itself without bound. */
    if (argc >= 2) {
        embk_write(3, argv[1], embk_strlen(argv[1]));   /* fd 3: parent-redirected */
        embk_exit((int)argc);                            /* parent asserts == 2 */
    }

    /* Top-level role: run the whole native-primitive suite, exit 16 iff all
     * passed (the sentinel `test ring3 threads` checks). */
    int ok = 1;
    ok &= thread_test();
    ok &= spawn_test();
    ok &= sbrk_test();
    embk_exit(ok ? 16 : 1);
}
