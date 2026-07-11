/* user/embk.h — the EmbLink SDK.
 *
 * newlib gives an EmbLink program the standard C library (printf, malloc,
 * string.h, ...). It CANNOT give it the things POSIX doesn't model but this
 * OS is built around: spawn()-based process creation with file-action
 * redirection, ring-3 threads with join, cooperative yield, capability
 * handles, and the native readdir/stat surface. This header is that layer --
 * the typed, documented EmbLink-native API, sitting directly on the raw
 * int-0x80 ABI (user/embk_syscall.h).
 *
 * HEADER-ONLY (every function is `static inline`): usable from a freestanding
 * program (user/init.c -- its own _start, no libc) AND from a newlib program
 * (link it alongside libc for the best of both: printf/malloc from newlib,
 * spawn/threads from here). No separate .o to link, no libc dependency.
 *
 * Return convention (inherited from the kernel): >= 0 is success (an fd, a
 * handle, a tid, a byte count, a break address); a small negative value is
 * -EMBK_* (see kernel/include/errno.h). Callers test `ret < 0`. This is the
 * RAW kernel convention -- unlike user/syscalls.c's POSIX stubs, these do NOT
 * set errno; they hand the negative code straight back, which is what a
 * freestanding program wants (no libc errno to touch). */

#ifndef __EMBK_H__
#define __EMBK_H__

#include <stdint.h>
#include <stddef.h>
#include "embk_syscall.h"

/* ==================================================================== */
/* open() flags -- the KERNEL's values (kernel/fs/fd.h). A newlib program  */
/* that wants POSIX open() uses <fcntl.h>+libc; these are for freestanding */
/* callers going straight through embk_open() (and match what the kernel   */
/* actually interprets, unlike newlib's differently-numbered O_* macros).   */
/* ==================================================================== */
#define EMBK_O_RDONLY   0x0000
#define EMBK_O_WRONLY   0x0001
#define EMBK_O_RDWR     0x0002
#define EMBK_O_CREAT    0x0040
#define EMBK_O_EXCL     0x0080
#define EMBK_O_TRUNC    0x0200
#define EMBK_O_APPEND   0x0400

/* Directory-entry / stat type tags (kernel VFS_DT_*). */
#define EMBK_DT_UNKNOWN 0
#define EMBK_DT_REG     1
#define EMBK_DT_DIR     2
#define EMBK_DT_LNK     3

/* Seek origins for embk_lseek() (kernel vfs_fd_seek convention). */
#define EMBK_SEEK_SET   0
#define EMBK_SEEK_CUR   1
#define EMBK_SEEK_END   2

/* ==================================================================== */
/* Types shared with the kernel by value (hand-synced -- no shared kernel  */
/* header). Same compiler + default layout on both sides, so a copy_to/    */
/* from_user of one of these matches byte-for-byte.                        */
/* ==================================================================== */

/* One spawn() file action: currently only "open PATH onto TARGET_FD in the
 * child before it runs" (EMBK_SPAWN_ACTION_OPEN). Mirrors the kernel's
 * struct spawn_file_action (kernel/process/spawn.h). */
#define EMBK_SPAWN_ACTION_OPEN 1
struct embk_spawn_file_action {
    unsigned char kind;         /* EMBK_SPAWN_ACTION_OPEN */
    int           target_fd;    /* fd the opened file lands on in the child */
    char          path[256];    /* NUL-terminated path to open */
    int           flags;        /* EMBK_O_* */
    unsigned int  mode;         /* creation mode if EMBK_O_CREAT */
};

/* One directory entry from embk_readdir() (kernel struct sys_dirent). */
struct embk_dirent {
    uint64_t ino;               /* fs-private object id */
    uint8_t  type;              /* EMBK_DT_* */
    char     name[59];          /* NUL-terminated (truncated if longer) */
};

/* File metadata from embk_stat()/embk_fstat() (kernel struct vfs_stat). */
struct embk_stat {
    uint8_t  type;              /* EMBK_DT_* */
    uint32_t mode;              /* POSIX-shaped st_mode */
    uint64_t size;              /* logical size in bytes */
    uint64_t nlink;             /* hard-link count */
};

/* ==================================================================== */
/* File descriptors / VFS                                                 */
/* ==================================================================== */

static inline int64_t embk_write(int fd, const void *buf, size_t len) {
    return embk_syscall3(EMBK_SYS_write, fd, (int64_t)(intptr_t)buf, (int64_t)len);
}
static inline int64_t embk_read(int fd, void *buf, size_t len) {
    return embk_syscall3(EMBK_SYS_read, fd, (int64_t)(intptr_t)buf, (int64_t)len);
}
static inline int64_t embk_open(const char *path, int flags, unsigned mode) {
    return embk_syscall3(EMBK_SYS_open, (int64_t)(intptr_t)path, flags, mode);
}
static inline int64_t embk_close(int fd) {
    return embk_syscall1(EMBK_SYS_close, fd);
}
static inline int64_t embk_lseek(int fd, int64_t offset, int whence) {
    return embk_syscall3(EMBK_SYS_lseek, fd, offset, whence);
}
static inline int64_t embk_stat(const char *path, struct embk_stat *out) {
    return embk_syscall2(EMBK_SYS_stat, (int64_t)(intptr_t)path, (int64_t)(intptr_t)out);
}
static inline int64_t embk_fstat(int fd, struct embk_stat *out) {
    return embk_syscall2(EMBK_SYS_fstat, fd, (int64_t)(intptr_t)out);
}
/* Read up to `max` directory entries of `path` into `out`; returns the count
 * actually written, or -EMBK_*. Not resumable -- one call walks the whole
 * directory (see the kernel's sys_readdir comment). */
static inline int64_t embk_readdir(const char *path, struct embk_dirent *out, uint32_t max) {
    return embk_syscall3(EMBK_SYS_readdir, (int64_t)(intptr_t)path,
                         (int64_t)(intptr_t)out, (int64_t)max);
}

/* ==================================================================== */
/* Memory                                                                 */
/* ==================================================================== */

/* Adjust the heap break by `incr` bytes; returns the PREVIOUS break (newly
 * available region starts there) or -EMBK_* on failure. A newlib program
 * gets malloc() on top of this for free; a freestanding one can carve its
 * own allocator out of it. */
static inline int64_t embk_sbrk(int64_t incr) {
    return embk_syscall1(EMBK_SYS_sbrk, incr);
}

/* ==================================================================== */
/* Processes -- spawn()-based, NOT fork()/exec(). This is the model POSIX  */
/* can't express, and the reason this SDK exists.                         */
/* ==================================================================== */

/* Launch `path` as a new process. `argv` is NULL-terminated (argv[0] is
 * conventionally the program name); argc is counted here so callers never
 * touch the raw ABI. `actions`/`n_actions` pre-open files onto specific fds
 * in the child before it runs (may be NULL/0). Returns an opaque per-caller
 * HANDLE (not a raw pid -- capability-scoped, only this process can name the
 * child via embk_wait/embk_kill), or -EMBK_*. */
static inline int64_t embk_spawn(const char *path, char *const argv[],
                                 const struct embk_spawn_file_action *actions,
                                 int n_actions) {
    int argc = 0;
    if (argv) {
        while (argv[argc]) argc++;
    }
    return embk_syscall5(EMBK_SYS_spawn, (int64_t)(intptr_t)path,
                         (int64_t)(intptr_t)argv, argc,
                         (int64_t)(intptr_t)actions, n_actions);
}

/* Block until the child named by `handle` exits; returns its exit code (or
 * -1 if it was killed), and frees the handle. -EMBK_* for a bad handle. */
static inline int64_t embk_wait(int handle) {
    return embk_syscall1(EMBK_SYS_wait, handle);
}

/* Uncatchably terminate the child named by `handle`. The handle stays valid
 * for a following embk_wait() to collect the exit status. */
static inline int64_t embk_kill(int handle) {
    return embk_syscall1(EMBK_SYS_kill, handle);
}

static inline int64_t embk_getpid(void) {
    return embk_syscall0(EMBK_SYS_getpid);
}

/* End the calling PROCESS (all its threads) with `code`. Never returns. */
static inline void embk_exit(int code) {
    embk_syscall1(EMBK_SYS_exit, code);
    for (;;) { }
}

/* ==================================================================== */
/* Threads -- additional ring-3 threads of the CALLING process, sharing    */
/* its address space (the pthread-shaped primitive newlib has no syscall    */
/* for on a bare kernel).                                                  */
/* ==================================================================== */

/* Start `entry` as a new thread of this process; `arg` arrives as entry's
 * first parameter (RDI). Returns a tid (>= 0) or -EMBK_*. The tid names a
 * thread only within THIS process. */
static inline int64_t embk_thread_create(void (*entry)(long arg), long arg) {
    return embk_syscall2(EMBK_SYS_thread_create, (int64_t)(intptr_t)entry, arg);
}

/* Block until thread `tid` of this process exits; returns its exit code. */
static inline int64_t embk_thread_join(int tid) {
    return embk_syscall1(EMBK_SYS_thread_join, tid);
}

/* End the calling THREAD (not the whole process, unless it's the last one)
 * with `code`. Never returns. */
static inline void embk_thread_exit(int code) {
    embk_syscall1(EMBK_SYS_thread_exit, code);
    for (;;) { }
}

/* ==================================================================== */
/* Scheduling                                                             */
/* ==================================================================== */

/* Voluntarily give up the rest of this timeslice to the next runnable
 * thread. Cooperative complement to the timer's preemption. */
static inline void embk_yield(void) {
    embk_syscall0(EMBK_SYS_yield);
}

/* ==================================================================== */
/* Tiny freestanding helpers (no libc). A newlib program has string.h and  */
/* ignores these; a freestanding one (user/init.c) leans on them.          */
/* ==================================================================== */

static inline size_t embk_strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}
static inline int embk_streq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}
/* Convenience: write a NUL-terminated string to an fd. */
static inline int64_t embk_puts(int fd, const char *s) {
    return embk_write(fd, s, embk_strlen(s));
}

#endif /* __EMBK_H__ */
