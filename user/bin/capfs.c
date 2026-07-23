/* capfs.c -- witness for the FILESYSTEM gate. Tries to open a file that exists
 * (/hello.txt) and reports the outcome, exactly like capgpu does for GPU:
 *   2  open granted (this process holds EMBK_CAP_FILESYSTEM)
 *   0  open refused with EPERM (it does not)
 *   3  some other failure (e.g. the file is missing) -- distinct so it can't be
 *      mistaken for the clean gate denial
 * Note it still has stdout: fds 0/1/2 are installed at spawn, not via open, so
 * a process without FILESYSTEM keeps its console -- it just cannot open more. */
#include "embk.h"

int main(void) {
    int fd = embk_open("/hello.txt", EMBK_O_RDONLY, 0);
    if (fd >= 0) { embk_close(fd); return 2; }   /* granted */
    if (fd == -1) return 0;                       /* -EMBK_EPERM: gated */
    return 3;                                     /* other (missing file, ...) */
}
