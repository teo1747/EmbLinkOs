/* user/syscalls.c — the newlib retargeting layer for EmbLink OS.
 *
 * newlib is architecture-neutral C library code that bottoms out in a small,
 * fixed set of "system call" stubs the platform must provide; this file is
 * that set for EmbLink, each stub wrapping one of the kernel's int-0x80
 * syscalls (user/embk_syscall.h). Compiled and linked INTO a newlib program
 * (against the real <errno.h>/<sys/stat.h>/... from the cross toolchain), so
 * it uses newlib's own types and just fills them.
 *
 * NAMING: this toolchain's newlib is the REENTRANT build -- its _read_r/
 * _write_r/... wrappers call the BARE POSIX names (read, write, sbrk, ...),
 * not the _read/_write underscore forms. So the stubs here are the bare
 * names (they double as the public POSIX API), with the sole exception of
 * _exit, which is genuinely the underscore form everywhere (libc's exit()
 * flushes stdio then calls _exit()). A mismatch here shows up as an
 * "undefined reference to `read'" style LINK error, not a silent bug.
 *
 * Error convention: the kernel returns a small negative -EMBK_* code on
 * failure (values chosen to match POSIX errno, see kernel/include/errno.h),
 * so a failing stub sets `errno = -ret` and returns -1 with no translation
 * table. embk_is_err() (embk_syscall.h) draws the line at the Linux-style
 * [-4095,-1] error window, so real large results (an sbrk address, a byte
 * count) are never mistaken for error codes.
 *
 * Genuinely-absent operations (fork/execve/wait/link/unlink) fail with
 * ENOSYS rather than pretending: EmbLink's process model is spawn()-based
 * (no fork/exec), and the VFS public surface exposes no unlink/link yet.
 * Programs wanting EmbLink's native primitives (spawn, threads, handles)
 * call them directly, not through this POSIX-shaped layer. */

#include <errno.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/times.h>
#include <sys/time.h>
#include <sys/wait.h>

#include "embk_syscall.h"

/* --- kernel types replicated on the user side (no kernel headers here) ---
 * struct vfs_stat (kernel/fs/vfs.h) and the VFS_DT_* type tags, byte-for-
 * byte. Same compiler + default alignment on both sides, so the layout the
 * kernel copy_to_user()s matches what we read back. Hand-synced, like
 * user/init.c's struct spawn_file_action. */
#define EMBK_VFS_DT_UNKNOWN 0
#define EMBK_VFS_DT_REG     1
#define EMBK_VFS_DT_DIR     2
#define EMBK_VFS_DT_LNK     3
struct embk_vfs_stat {
    uint8_t  type;
    uint32_t mode;
    uint64_t size;
    uint64_t nlink;
};

/* The KERNEL's O_* flag bits (kernel/fs/fd.h) -- DELIBERATELY not the same
 * numeric values as newlib's <fcntl.h> O_* macros (newlib O_CREAT=0x200,
 * kernel O_CREAT=0x040, etc.). open() below translates one to the other;
 * passing newlib's bits through raw would have the kernel read O_CREAT as
 * O_TRUNC. Access mode (low 2 bits: RDONLY/WRONLY/RDWR = 0/1/2) is identical
 * on both sides and passes through unchanged. */
#define EMBK_O_CREAT   0x0040
#define EMBK_O_EXCL    0x0080
#define EMBK_O_TRUNC   0x0200
#define EMBK_O_APPEND  0x0400

/* newlib requires a definition of `environ`; nothing builds a real
 * environment yet, so it's a single NULL-terminated empty vector. */
static char *embk_empty_env[1] = { 0 };
char **environ = embk_empty_env;

/* Shared failure tail for the int-returning stubs: -EMBK_* -> errno + -1. */
static int embk_fail(int64_t ret) {
    errno = (int)(-ret);
    return -1;
}

/* Map the kernel's neutral struct vfs_stat onto newlib's struct stat. The
 * file TYPE comes from vs->type (VFS_DT_*, a stable kernel-defined tag),
 * never from vs->mode's own type bits -- so this stays correct even if the
 * kernel's internal S_IF* values ever diverged from newlib's. Permission
 * bits are the low 9 of vs->mode. */
static void embk_fill_stat(struct stat *st, const struct embk_vfs_stat *vs) {
    memset(st, 0, sizeof(*st));
    st->st_mode = (mode_t)(vs->mode & 0777);
    switch (vs->type) {
        case EMBK_VFS_DT_DIR: st->st_mode |= S_IFDIR; break;
        case EMBK_VFS_DT_LNK: st->st_mode |= S_IFLNK; break;
        case EMBK_VFS_DT_REG:
        default:              st->st_mode |= S_IFREG; break;
    }
    st->st_size    = (off_t)vs->size;
    st->st_nlink   = vs->nlink ? (nlink_t)vs->nlink : 1;
    st->st_blksize = 4096;
    st->st_blocks  = (blkcnt_t)((vs->size + 511) / 512);
    st->st_ino     = 0;   /* the fd/path stat path doesn't surface the oid */
}

/* ------------------------------------------------------------------ */
/* Process lifetime                                                    */
/* ------------------------------------------------------------------ */

void _exit(int code) {
    embk_syscall1(EMBK_SYS_exit, code);
    for (;;) { }   /* SYS_exit is noreturn in the kernel; satisfy the attribute */
}

pid_t getpid(void) {
    return (pid_t)embk_syscall0(EMBK_SYS_getpid);
}

/* No general signal delivery. This exists mainly so abort()/raise() actually
 * terminate instead of spinning: a fatal self-signal exits with the
 * conventional 128+signo status. sig 0 is POSIX's "just test that the target
 * exists" probe and must NOT kill -- report success for self. */
int kill(pid_t pid, int sig) {
    if (sig == 0 && pid == getpid()) {
        return 0;
    }
    if (pid == getpid()) {
        _exit(128 + sig);
    }
    errno = ENOSYS;
    return -1;
}

/* ------------------------------------------------------------------ */
/* File descriptors                                                    */
/* ------------------------------------------------------------------ */

/* Return type is int, not ssize_t: this newlib build defines
 * _READ_WRITE_RETURN_TYPE as int (sys/config.h fallback), and read()/write()
 * are declared with it -- matching avoids a conflicting-types error. A single
 * read/write is chunked to <= SYSCALL_IO_CHUNK in the kernel anyway, so int
 * range is never a real limit here. */
int write(int fd, const void *buf, size_t len) {
    int64_t ret = embk_syscall3(EMBK_SYS_write, fd, (int64_t)(intptr_t)buf, (int64_t)len);
    if (embk_is_err(ret)) return embk_fail(ret);
    return (int)ret;
}

int read(int fd, void *buf, size_t len) {
    int64_t ret = embk_syscall3(EMBK_SYS_read, fd, (int64_t)(intptr_t)buf, (int64_t)len);
    if (embk_is_err(ret)) return embk_fail(ret);
    return (int)ret;
}

int open(const char *name, int flags, ...) {
    /* Only consume the variadic mode argument when O_CREAT is set -- reading
     * it unconditionally would be UB for the common open(path, O_RDONLY). */
    int mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
    }

    /* Translate newlib's O_* bit layout to the kernel's (see EMBK_O_* above). */
    int kflags = flags & 0x3;   /* access mode: identical on both sides */
    if (flags & O_CREAT)  kflags |= EMBK_O_CREAT;
    if (flags & O_EXCL)   kflags |= EMBK_O_EXCL;
    if (flags & O_TRUNC)  kflags |= EMBK_O_TRUNC;
    if (flags & O_APPEND) kflags |= EMBK_O_APPEND;

    int64_t ret = embk_syscall3(EMBK_SYS_open, (int64_t)(intptr_t)name, kflags, mode);
    if (embk_is_err(ret)) return embk_fail(ret);
    return (int)ret;
}

int close(int fd) {
    /* stdio fds have no kernel vnode (see sys_write); closing them is a
     * successful no-op rather than an fd-layer error. */
    if (fd == 0 || fd == 1 || fd == 2) {
        return 0;
    }
    int64_t ret = embk_syscall1(EMBK_SYS_close, fd);
    if (embk_is_err(ret)) return embk_fail(ret);
    return 0;
}

off_t lseek(int fd, off_t offset, int whence) {
    int64_t ret = embk_syscall3(EMBK_SYS_lseek, fd, (int64_t)offset, whence);
    if (embk_is_err(ret)) return (off_t)embk_fail(ret);
    return (off_t)ret;
}

int isatty(int fd) {
    if (fd == 0 || fd == 1 || fd == 2) {
        return 1;   /* stdin/stdout/stderr are the serial console */
    }
    errno = ENOTTY;
    return 0;
}

int fstat(int fd, struct stat *st) {
    /* stdio fds aren't real vnodes -- report a character device so newlib
     * line-buffers stdout instead of trying to size a block-buffer from a
     * (nonexistent) file length. The standard newlib _fstat-on-a-tty case. */
    if (fd == 0 || fd == 1 || fd == 2) {
        memset(st, 0, sizeof(*st));
        st->st_mode = S_IFCHR;
        st->st_blksize = 1024;
        return 0;
    }

    struct embk_vfs_stat vs;
    int64_t ret = embk_syscall2(EMBK_SYS_fstat, fd, (int64_t)(intptr_t)&vs);
    if (embk_is_err(ret)) return embk_fail(ret);
    embk_fill_stat(st, &vs);
    return 0;
}

int stat(const char *path, struct stat *st) {
    struct embk_vfs_stat vs;
    int64_t ret = embk_syscall2(EMBK_SYS_stat, (int64_t)(intptr_t)path,
                                (int64_t)(intptr_t)&vs);
    if (embk_is_err(ret)) return embk_fail(ret);
    embk_fill_stat(st, &vs);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Memory                                                              */
/* ------------------------------------------------------------------ */

void *sbrk(ptrdiff_t incr) {
    int64_t ret = embk_syscall1(EMBK_SYS_sbrk, (int64_t)incr);
    if (embk_is_err(ret)) {
        errno = (int)(-ret);
        return (void *)-1;   /* malloc's sentinel for "out of memory" */
    }
    return (void *)(uintptr_t)ret;
}

/* ------------------------------------------------------------------ */
/* Time                                                                */
/* ------------------------------------------------------------------ */

int gettimeofday(struct timeval *tv, void *tz) {
    (void)tz;
    uint64_t out[2];   /* [0]=seconds, [1]=microseconds (always 0, RTC is 1s) */
    int64_t ret = embk_syscall1(EMBK_SYS_gettimeofday, (int64_t)(intptr_t)out);
    if (embk_is_err(ret)) return embk_fail(ret);
    if (tv) {
        tv->tv_sec  = (time_t)out[0];
        tv->tv_usec = (suseconds_t)out[1];
    }
    return 0;
}

/* No per-process CPU accounting yet; report zero elapsed rather than fail, so
 * clock()/times() are well-defined (they just never advance). */
clock_t times(struct tms *buf) {
    if (buf) {
        buf->tms_utime = buf->tms_stime = buf->tms_cutime = buf->tms_cstime = 0;
    }
    return (clock_t)0;
}

/* ------------------------------------------------------------------ */
/* Not modeled on EmbLink (honest ENOSYS, not silent no-ops)           */
/* ------------------------------------------------------------------ */

pid_t fork(void) {
    errno = ENOSYS;   /* EmbLink spawns whole ELFs; there is no fork() */
    return -1;
}

int execve(const char *path, char *const argv[], char *const envp[]) {
    (void)path; (void)argv; (void)envp;
    errno = ENOSYS;   /* spawn() replaces fork+exec; no in-place image swap */
    return -1;
}

pid_t wait(int *status) {
    (void)status;
    errno = ENOSYS;   /* the kernel's wait is handle-based, not wait-any-child */
    return -1;
}

int link(const char *oldpath, const char *newpath) {
    (void)oldpath; (void)newpath;
    errno = ENOSYS;   /* VFS public surface exposes no link op yet */
    return -1;
}

int unlink(const char *path) {
    (void)path;
    errno = ENOSYS;   /* VFS public surface exposes no unlink op yet */
    return -1;
}
