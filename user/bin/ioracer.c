/* ioracer.c -- contend for the block layer's shared DMA bounce buffer, and
 * notice if it hands back somebody else's sectors.
 *
 * WHY THIS EXISTS. block.c has exactly one bounce buffer, a single 32 KB array
 * in kernel BSS used whenever a caller's destination is not DMA-safe for the
 * device. On this kernel that is the COMMON case, not the exotic one: kmalloc
 * memory lives in the direct map (0xFFFF8000_...), the ATA driver needs
 * KERNEL_VIRTUAL_BASE (0xFFFFFFFF80_...), so every read into a kmalloc'd
 * buffer -- which is most filesystem I/O -- bounces.
 *
 * A lock was added for that buffer, and the TODO entry recording it was honest
 * about what it had not done:
 *
 *     "The race itself is NOT covered by a test. Every test above is
 *      single-threaded, so the lock is never contended -- they prove no
 *      deadlock and no regression, not that the corruption is gone."
 *
 * This is that missing test. It is a userspace program because the race needs
 * genuinely concurrent readers, and processes are how this OS gets them.
 *
 * WHAT THE BUG WOULD LOOK LIKE. Two readers interleaving inside the bounce
 * path memcpy over each other and each return the other's sectors. That is
 * SILENT: no fault, no error code, just bytes from the wrong file. So this
 * program cannot check "did the read succeed" -- it has to check the CONTENT,
 * byte for byte, against a pattern that could only have come from its own
 * file. Every process reads a DIFFERENT file with a DIFFERENT fill byte, so a
 * single swapped sector is unmistakable: you get 'B' where 'A' belongs, and
 * the report says which offset and what arrived.
 *
 * usage: ioracer <path> <fill-char> [iterations]
 * exit:  0 = every byte of every pass was ours
 *        1 = usage/open/read error (a real failure, but NOT the race)
 *        2 = CONTENT MISMATCH -- the thing this program exists to catch
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#define CHUNK 4096

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: ioracer <path> <fill-char> [iterations]\n");
        return 1;
    }
    const char *path = argv[1];
    unsigned char want = (unsigned char)argv[2][0];
    int iters = (argc > 3) ? atoi(argv[3]) : 4;
    if (iters <= 0) iters = 4;

    static unsigned char buf[CHUNK];
    unsigned long long checked = 0;

    for (int it = 0; it < iters; it++) {
        /* Reopen every pass. A single open would let the file's pages settle
         * into a cache and stop reaching the block layer at all -- which would
         * make this program pass without ever touching the buffer it is here
         * to contend for. */
        int fd = open(path, O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "ioracer: %s: cannot open (pass %d)\n", path, it);
            return 1;
        }
        for (;;) {
            ssize_t n = read(fd, buf, sizeof buf);
            if (n < 0) {
                fprintf(stderr, "ioracer: %s: read error at %llu\n", path, checked);
                close(fd);
                return 1;
            }
            if (n == 0) break;
            for (ssize_t i = 0; i < n; i++) {
                if (buf[i] != want) {
                    /* The whole point. Report enough to identify the intruder:
                     * which file we were reading, what we expected, what we got
                     * (the other process's fill byte, if the buffer raced), and
                     * where -- so the failure names its own cause. */
                    fprintf(stderr,
                            "ioracer: MISMATCH in %s at byte %llu: got 0x%02X ('%c'), "
                            "want 0x%02X ('%c') -- another reader's bytes\n",
                            path, checked + (unsigned long long)i,
                            buf[i], (buf[i] >= 32 && buf[i] < 127) ? buf[i] : '?',
                            want, want);
                    close(fd);
                    return 2;
                }
            }
            checked += (unsigned long long)n;
        }
        close(fd);
    }

    printf("ioracer: %s OK -- %llu bytes verified over %d pass(es), all '%c'\n",
           path, checked, iters, want);
    return 0;
}
