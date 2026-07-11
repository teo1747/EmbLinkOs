/* user/hello.c — a genuine newlib program: real main(), real libc. Proves the
 * retargeting layer (user/syscalls.c) satisfies everything newlib's stdio,
 * malloc, and time code bottom out in. Exercises, in order:
 *   printf         -> stdout -> _write/_fstat/_isatty/_sbrk
 *   malloc/free    -> _sbrk
 *   snprintf       -> pure libc (no syscall), sanity that libc itself links
 *   time()         -> _gettimeofday
 * Exit status is the number of checks that PASSED, so the launcher can assert
 * on it (see the run harness). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "embk.h"   /* the EmbLink SDK: native primitives newlib can't give */

/* A native EmbLink thread of THIS (newlib) process. Writes via embk_puts (a
 * direct write syscall) rather than printf on purpose: newlib's stdio isn't
 * reentrant, and this thread runs concurrently with main until the join.
 * Sets a shared flag main checks after joining. */
static volatile int embk_thread_flag = 0;
static void demo_thread(long arg)
{
    embk_puts(1, "  [embk thread] running as a native thread of the newlib process\n");
    embk_thread_flag = (int)arg;
    embk_thread_exit(0);
}

int main(int argc, char **argv)
{
    int passed = 0;

    printf("hello from newlib! argc=%d argv0=%s\n",
           argc, (argc > 0 && argv && argv[0]) ? argv[0] : "(none)");
    passed++;   /* if this printed, stdout/_write/_fstat/_isatty all work */

    /* malloc -> _sbrk: allocate, write a pattern, read it back. */
    size_t n = 4096;
    char *buf = malloc(n);
    if (buf) {
        memset(buf, 0x5A, n);
        int ok = 1;
        for (size_t i = 0; i < n; i++) {
            if ((unsigned char)buf[i] != 0x5A) { ok = 0; break; }
        }
        free(buf);
        if (ok) {
            /* %zu and %llx exercise the C99 size modifier + long-long support
             * this newlib was rebuilt with (--enable-newlib-io-c99-formats /
             * --enable-newlib-io-long-long). On the stock toolchain newlib
             * these printed a literal "zu"/"llx". */
            printf("malloc/free of %zu bytes (0x%llx): OK\n",
                   n, (unsigned long long)0xDEADBEEFCAFEULL);
            passed++;
        } else {
            printf("malloc buffer readback: FAIL\n");
        }
    } else {
        printf("malloc returned NULL\n");
    }

    /* Pure-libc formatting -- no syscall, just confirms libc itself is sound. */
    char sb[64];
    int wrote = snprintf(sb, sizeof(sb), "%d-%x-%s", 42, 0xBEEF, "ok");
    if (wrote > 0 && strcmp(sb, "42-beef-ok") == 0) {
        printf("snprintf: OK (\"%s\")\n", sb);
        passed++;
    } else {
        printf("snprintf: FAIL (\"%s\")\n", sb);
    }

    /* time() -> _gettimeofday -> the CMOS RTC. Just assert it's plausibly a
     * real wall-clock (past 2020-01-01), not 0/garbage. */
    time_t now = time(NULL);
    if (now > 1577836800) {   /* 2020-01-01T00:00:00Z */
        printf("time() = %ld (plausible wall clock)\n", (long)now);
        passed++;
    } else {
        printf("time() = %ld (implausible)\n", (long)now);
    }

    /* EmbLink SDK from inside a newlib program: spin up a real ring-3 thread
     * of this process, join it, and confirm it ran and shared our memory.
     * This is what the SDK adds on top of libc -- POSIX/newlib has no way to
     * do this on a bare kernel. */
    long tid = embk_thread_create(demo_thread, 7);
    if (tid >= 0 && embk_thread_join((int)tid) == 0 && embk_thread_flag == 7) {
        printf("embk thread create/join: OK (tid=%ld)\n", tid);
        passed++;
    } else {
        printf("embk thread create/join: FAIL\n");
    }

    printf("hello: %d/5 checks passed\n", passed);
    return passed;
}
