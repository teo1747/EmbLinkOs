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

/* newlib's <sys/features.h> only defines _POSIX_TIMERS for targets it knows
 * (RTEMS/Cygwin), never for bare x86_64-elf, so <time.h> hides clock_gettime()
 * and the CLOCK_* ids behind `#if defined(_POSIX_TIMERS)` -- no feature-test
 * macro unlocks them. We define it ourselves because, below, we make the claim
 * TRUE: this libc does provide the POSIX monotonic/realtime clocks. Must come
 * before any libc header.
 *
 * _POSIX_MONOTONIC_CLOCK is a SECOND, separate gate: without it <time.h> still
 * hides CLOCK_MONOTONIC. (CLOCK_MONOTONIC_RAW/BOOTTIME sit behind __GNU_VISIBLE
 * and stay hidden -- they're GNU extensions, so the switch below serves them
 * only #ifdef'd, when something asks for GNU visibility.) */
#define _POSIX_TIMERS 200809L
#define _POSIX_MONOTONIC_CLOCK 200809L

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
#include <time.h>          /* clock_gettime(), CLOCK_* -- see _POSIX_TIMERS above */
#include <stdlib.h>        /* malloc/calloc/free -- opendir()'s snapshot */
#include <dirent.h>        /* DIR, struct dirent, DT_* -- our sys/dirent.h override */
#include <utime.h>         /* struct utimbuf -- our sys/utime.h override */
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
#define EMBK_VFS_DT_CHAR    5   /* character device (the console backing fds 0-2) */
#define EMBK_VFS_DT_FIFO    6   /* a pipe end -- what sval's structured-pipeline
                                 * detection looks for (S_ISFIFO on fd 3 / fd 0) */
struct embk_vfs_stat {
    uint8_t  type;
    uint32_t mode;
    uint64_t size;
    uint64_t mtime;   /* seconds since epoch; 0 = fs doesn't track it.
                       * Mirrors kernel struct vfs_stat FIELD-FOR-FIELD
                       * (raw copy_to_user) -- keep the position. */
    uint64_t nlink;
};

/* Mirrors kernel struct sys_dirent FIELD-FOR-FIELD (raw copy_to_user), same
 * contract as embk_vfs_stat above -- keep the field order and the name[59]. */
struct embk_vfs_dirent {
    uint64_t ino;
    uint8_t  type;      /* EMBK_VFS_DT_* */
    char     name[59];  /* NUL-terminated; the kernel truncates longer names */
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

/* The environment. newlib requires `environ`; getenv() walks it.
 *
 * crt0's start_c() OVERWRITES this with the vector the kernel delivered in RDX
 * before any ctor or main runs. The value here is only the fallback for a child
 * whose parent passed NO environment -- the EmbLink default, since nothing is
 * inherited unless a parent names it explicitly (kernel spawn.h). An empty
 * vector, not NULL: getenv() must be able to walk it and answer "unset", and
 * every caller expects environ to be dereferenceable.
 *
 * NOT static: crt0.c references it to install the kernel's vector. */
char *embk_empty_env[1] = { 0 };
char **environ = embk_empty_env;

/* Kernel error number -> newlib errno.
 *
 * THESE TWO NUMBERINGS ARE NOT THE SAME. The kernel numbers its errors
 * LINUX-style (kernel/include/errno.h); newlib has its own assignments, and they
 * agree only up to ~34 by shared Unix ancestry. Above that they diverge, so the
 * old `errno = -ret` silently reported:
 *
 *     kernel ENOSYS(38)       -> app read EL2NSYNC
 *     kernel ENAMETOOLONG(36) -> app read EIDRM
 *     kernel ECANCELED(125)   -> app read EADDRNOTAVAIL
 *
 * 12 of the kernel's 42 codes were affected. The hand-written stubs in this file
 * were never wrong (they assign newlib's own constants directly), which is
 * exactly why this hid: the paths that DID get it wrong are the ones that pass a
 * kernel code through, and their errno is usually only inspected when something
 * has already gone wrong.
 *
 * The mapping is by NAME, not by number -- that is the whole point. Codes the
 * kernel and newlib already agree on are omitted; anything unlisted passes
 * through unchanged. Kernel values are inlined (userspace does not, and should
 * not, include the kernel's errno.h) with the name beside each so a reader can
 * check both sides. */
static int embk_errno_from_kernel(int64_t ret) {
    switch ((int)(-ret)) {
    case 35:  return EDEADLK;        /* EMBK_EDEADLK      */
    case 36:  return ENAMETOOLONG;   /* EMBK_ENAMETOOLONG */
    case 37:  return ENOLCK;         /* EMBK_ENOLCK       */
    case 38:  return ENOSYS;         /* EMBK_ENOSYS       */
    case 39:  return ENOTEMPTY;      /* EMBK_ENOTEMPTY    */
    case 40:  return ELOOP;          /* EMBK_ELOOP        */
    case 75:  return EOVERFLOW;      /* EMBK_EOVERFLOW    */
    case 84:  return EILSEQ;         /* EMBK_EILSEQ       */
    case 90:  return EMSGSIZE;       /* EMBK_EMSGSIZE     */
    case 95:  return ENOTSUP;        /* EMBK_ENOTSUP      */
    case 110: return ETIMEDOUT;      /* EMBK_ETIMEDOUT    */
    case 125: return ECANCELED;      /* EMBK_ECANCELED    */
    default:  return (int)(-ret);    /* agreed by shared Unix ancestry */
    }
}

/* Shared failure tail for the int-returning stubs: -EMBK_* -> errno + -1. */
static int embk_fail(int64_t ret) {
    errno = embk_errno_from_kernel(ret);
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
        case EMBK_VFS_DT_DIR:  st->st_mode |= S_IFDIR; break;
        case EMBK_VFS_DT_LNK:  st->st_mode |= S_IFLNK; break;
        case EMBK_VFS_DT_CHAR: st->st_mode |= S_IFCHR; break;
        case EMBK_VFS_DT_FIFO: st->st_mode |= S_IFIFO; break;
        case EMBK_VFS_DT_REG:
        default:               st->st_mode |= S_IFREG; break;
    }
    st->st_size    = (off_t)vs->size;
    st->st_mtime   = (time_t)vs->mtime;   /* 0 = unknown (FAT32/epfs) */
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

/* ------------------------------------------------------------------ */
/* Path normalisation                                                  */
/* ------------------------------------------------------------------ */

/* Must match the kernel's SYSCALL_PATH_MAX (syscall.c): it copies the path into
 * a buffer of exactly this size, so anything longer is rejected there anyway --
 * better to say ENAMETOOLONG here than to send a path we know will fail. */
#define EMBK_PATH_MAX 256

/* Resolve a caller's path to the ABSOLUTE form the kernel requires.
 *
 * EmbLink's VFS is absolute-only -- kernel/fs/vfs.c: `if (path[0] != '/')
 * return -EMBK_EINVAL;` ("v1 only supports absolute paths there is no
 * per-process cwd yet"). This libc reports getcwd() == "/" (see the Working
 * directory section below), so a relative name MUST mean "/name" for that model
 * to hold together. Until this existed, getcwd() said "/" while open("foo")
 * failed with EINVAL -- not a model, a contradiction.
 *
 * It broke real software, which is how it was found: CPython's frozen
 * getpath.py probes a relative "pyvenv.cfg" inside
 * `except (FileNotFoundError, PermissionError)`. Our EINVAL is neither, so it
 * escaped that handler and killed interpreter startup, where the ENOENT this
 * now produces is simply caught and skipped.
 *
 * Normalising HERE, in the POSIX veneer, is deliberate: the kernel keeps its
 * clean absolute-only contract (the native model), and POSIX-shaped behaviour
 * stays the libc's job. '.'/'..' need no work -- vfs_resolve already handles
 * them in its path layer, so "./x" -> "/./x" resolves correctly.
 *
 * Returns 0 with `out` filled, or -1 with errno set.
 */
/* THE WORKING DIRECTORY.
 *
 * Deliberately a LIBC fact, not a kernel one. kernel/fs/vfs.c is absolute-only
 * on purpose ("the ONLY path parser in the kernel"), and the precedent is
 * already set: when CPython needed relative paths the fix went HERE, to keep
 * that contract intact and put the POSIX veneer where the rest of the POSIX
 * veneer lives. So the kernel never sees a relative path -- everything below
 * hands it an absolute, normalized one.
 *
 * PER-PROCESS COMES FREE: every process has its own copy of this libc's data.
 * There is no kernel state to add, no syscall, and no way for one process's cwd
 * to leak into another's -- which is the property a kernel implementation would
 * have had to work to guarantee.
 *
 * NOT INHERITED, like everything else here: a child starts at "/" unless its
 * parent NAMES a starting directory, by passing PWD in the environment (crt0
 * seeds this from it). Same rule as file-actions and envp -- see the kernel's
 * spawn.h. A shell that wants `cd` to stick for its children passes PWD; one
 * that doesn't, doesn't. */
static char g_cwd[EMBK_PATH_MAX] = "/";

/* Make `in` absolute against g_cwd (if relative) and NORMALIZE it: "." dropped,
 * ".." pops a component. Output is always absolute and never ends in '/' except
 * for the root itself.
 *
 * The normalization is not decoration. Absolute paths used to pass through
 * untouched, so "/a/../b" reached the kernel verbatim and failed at a ".."
 * component the VFS has no rule for. git walks UP to find a repo root -- ".." is
 * its normal idiom, not an edge case. */
static int path_abs(const char *in, char *out, size_t out_sz) {
    if (!in) {
        errno = EFAULT;
        return -1;
    }
    if (in[0] == '\0') {
        /* POSIX: an empty path is ENOENT -- NOT the root. Prepending '/' would
         * silently turn open("") into open("/"), handing back a success where
         * the caller expects a failure. */
        errno = ENOENT;
        return -1;
    }

    /* Join first, into a scratch big enough for cwd + '/' + name. */
    char raw[EMBK_PATH_MAX * 2];
    if (in[0] == '/') {
        if (strlen(in) >= sizeof raw) { errno = ENAMETOOLONG; return -1; }
        strcpy(raw, in);
    } else {
        size_t cl = strlen(g_cwd);
        if (cl + 1 + strlen(in) >= sizeof raw) { errno = ENAMETOOLONG; return -1; }
        strcpy(raw, g_cwd);
        if (cl > 0 && raw[cl - 1] != '/') strcat(raw, "/");
        strcat(raw, in);
    }

    /* Normalize segment by segment. ".." at the root stays at the root (POSIX:
     * "/.." is "/"), rather than walking off the front of the buffer. */
    if (out_sz < 2) { errno = ENAMETOOLONG; return -1; }
    size_t o = 0;
    out[o++] = '/';
    const char *p = raw;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *seg = p;
        size_t n = 0;
        while (p[n] && p[n] != '/') n++;
        p += n;

        if (n == 1 && seg[0] == '.') continue;
        if (n == 2 && seg[0] == '.' && seg[1] == '.') {
            while (o > 1 && out[o - 1] != '/') o--;   /* pop the last component */
            if (o > 1) o--;                            /* and its slash */
            continue;
        }
        if (o > 1) {
            if (o + 1 >= out_sz) { errno = ENAMETOOLONG; return -1; }
            out[o++] = '/';
        }
        if (o + n >= out_sz) { errno = ENAMETOOLONG; return -1; }
        for (size_t i = 0; i < n; i++) out[o++] = seg[i];
    }
    out[o] = '\0';
    return 0;
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

    char abs[EMBK_PATH_MAX];
    if (path_abs(name, abs, sizeof abs) != 0) return -1;   /* errno set */

    int64_t ret = embk_syscall3(EMBK_SYS_open, (int64_t)(intptr_t)abs, kflags, mode);
    if (embk_is_err(ret)) return embk_fail(ret);
    return (int)ret;
}

int close(int fd) {
    /* Fds 0/1/2 are REAL table slots now (console- or pipe-backed), so close
     * goes to the kernel like any other fd -- closing a pipe-backed stdio
     * slot is load-bearing (it's how a pipeline stage signals EOF early).
     * The old "stdio no-op" special case predates the fd rework. */
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
    /* HONEST now: ask the kernel what actually backs the fd. A console slot
     * fstats as a character device -> tty; a PIPE-backed stdio slot (the
     * shell redirected us into a pipeline) correctly reports NOT-a-tty --
     * which is the entire point of the structured-output convention. */
    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISCHR(st.st_mode)) {
        return 1;
    }
    errno = ENOTTY;
    return 0;
}

int fstat(int fd, struct stat *st) {
    /* No stdio special case anymore: fds 0/1/2 are real slots and the kernel
     * reports their true backing (VFS_DT_CHAR for the console -- which keeps
     * newlib line-buffering stdout, same behavior the old hardcode faked --
     * or VFS_DT_FIFO for a pipe, which sval's pipeline detection needs). */
    struct embk_vfs_stat vs;
    int64_t ret = embk_syscall2(EMBK_SYS_fstat, fd, (int64_t)(intptr_t)&vs);
    if (embk_is_err(ret)) return embk_fail(ret);
    embk_fill_stat(st, &vs);
    if (S_ISCHR(st->st_mode)) st->st_blksize = 1024;   /* tty: line-buffer scale */
    return 0;
}

int stat(const char *path, struct stat *st) {
    char abs[EMBK_PATH_MAX];
    if (path_abs(path, abs, sizeof abs) != 0) return -1;   /* errno set */

    struct embk_vfs_stat vs;
    int64_t ret = embk_syscall2(EMBK_SYS_stat, (int64_t)(intptr_t)abs,
                                (int64_t)(intptr_t)&vs);
    if (embk_is_err(ret)) return embk_fail(ret);
    embk_fill_stat(st, &vs);
    return 0;
}

/* lstat() == stat(), and here that is exact rather than a convenient fudge:
 * NOTHING in this kernel dereferences a symlink. embkfs can store a link
 * (EMBKFS_DT_LNK -> VFS_DT_LNK, and stat reports S_IFLNK for it), but there is
 * no readlink() and no follow step anywhere in path resolution -- so stat()
 * already reports the link itself, which is precisely lstat()'s contract. With
 * no dereferencing to skip, the two cannot disagree.
 *
 * If link-following is ever implemented, this alias becomes a LIE and must be
 * split into a real no-follow call at the same time. */
int lstat(const char *path, struct stat *st) {
    return stat(path, st);
}

/* EmbLink tracks mtime but exposes no syscall to SET it, so utime() cannot do
 * its job. Fail with ENOSYS rather than returning 0 and silently ignoring the
 * request: a caller told "success" would believe the timestamp took. */
int utime(const char *path, const struct utimbuf *times) {
    (void)path; (void)times;
    errno = ENOSYS;
    return -1;
}

/* ------------------------------------------------------------------ */
/* Directories                                                         */
/* ------------------------------------------------------------------ */

/* The kernel's readdir is path-based, fixed-size and NOT resumable: one call
 * walks the whole directory (see sys_readdir's comment -- it predates having a
 * libc to match an ABI against). POSIX wants a stream, so opendir() takes a
 * SNAPSHOT and readdir() replays it. The consequence worth knowing: the
 * directory is sampled once, at opendir() time, and later changes are invisible
 * until the next opendir() -- POSIX explicitly leaves that unspecified. */
struct __dirstream {
    struct dirent *ents;   /* snapshot, `n` entries */
    size_t         n;
    size_t         pos;    /* next index readdir() will hand back */
};

/* EmbLink's native type enum -> the POSIX DT_* numbers. The two schemes agree
 * on NOTHING (native REG=1 vs DT_REG=8), so this table is load-bearing. */
static unsigned char dt_from_native(uint8_t t) {
    switch (t) {
    case EMBK_VFS_DT_REG:  return DT_REG;
    case EMBK_VFS_DT_DIR:  return DT_DIR;
    case EMBK_VFS_DT_LNK:  return DT_LNK;
    case EMBK_VFS_DT_CHAR: return DT_CHR;
    case EMBK_VFS_DT_FIFO: return DT_FIFO;
    default:
        /* Includes VFS_DT_ENDPOINT (4): an EmbLink IPC endpoint has no POSIX
         * analogue, and DT_SOCK would misdescribe it. DT_UNKNOWN is always a
         * legal answer and makes a caller fall back to stat(), which is exactly
         * the behaviour we want for a native object POSIX has no word for. */
        return DT_UNKNOWN;
    }
}

DIR *opendir(const char *path) {
    /* Normalise ONCE and use `abs` for both the stat below and the readdir
     * syscall: stat() would resolve its own copy, but the raw `path` is what
     * reaches the kernel further down, and a relative one would be rejected
     * there with EINVAL after stat() had already said the directory was fine. */
    char abs[EMBK_PATH_MAX];
    if (path_abs(path, abs, sizeof abs) != 0) return (DIR *)0;   /* errno set */

    /* Reject a non-directory up front, with the right errno and no allocation. */
    struct stat st;
    if (stat(abs, &st) != 0) return (DIR *)0;           /* errno already set */
    if (!S_ISDIR(st.st_mode)) { errno = ENOTDIR; return (DIR *)0; }

    /* The kernel TRUNCATES SILENTLY: when the buffer fills it stops collecting
     * and reports exactly max_entries, with no "there was more" signal. So a
     * SHORT count is the only proof we saw the whole directory -- whenever the
     * count comes back full, we must assume we lost entries, grow, and re-walk. */
    uint32_t cap = 64;
    for (;;) {
        struct embk_vfs_dirent *raw = (struct embk_vfs_dirent *)
            calloc((size_t)cap, sizeof *raw);
        if (!raw) { errno = ENOMEM; return (DIR *)0; }

        int64_t n = embk_syscall3(EMBK_SYS_readdir, (int64_t)(intptr_t)abs,
                                  (int64_t)(intptr_t)raw, (int64_t)cap);
        if (embk_is_err(n)) {
            int e = (int)(-n);
            free(raw);
            errno = e;
            return (DIR *)0;
        }

        if ((uint32_t)n == cap) {           /* full -> possibly truncated */
            free(raw);
            if (cap > (1u << 20)) {         /* ~1M entries; refuse to spin forever */
                errno = ENOMEM;
                return (DIR *)0;
            }
            cap *= 2;
            continue;
        }

        DIR *d = (DIR *)malloc(sizeof *d);
        if (!d) { free(raw); errno = ENOMEM; return (DIR *)0; }
        d->n   = (size_t)n;
        d->pos = 0;
        /* malloc(0) may legitimately return NULL; ask for 1 so an empty
         * directory still yields a valid, freeable DIR. */
        d->ents = (struct dirent *)calloc(d->n ? d->n : 1, sizeof *d->ents);
        if (!d->ents) { free(d); free(raw); errno = ENOMEM; return (DIR *)0; }

        for (size_t i = 0; i < d->n; i++) {
            d->ents[i].d_ino    = (ino_t)raw[i].ino;
            d->ents[i].d_type   = dt_from_native(raw[i].type);
            d->ents[i].d_reclen = (unsigned short)sizeof(struct dirent);
            /* raw name is already NUL-terminated in 59 bytes (kernel truncates
             * longer ones); bound the copy anyway rather than trust the kernel. */
            strncpy(d->ents[i].d_name, raw[i].name, sizeof d->ents[i].d_name - 1);
            d->ents[i].d_name[sizeof d->ents[i].d_name - 1] = '\0';
        }
        free(raw);
        return d;
    }
}

struct dirent *readdir(DIR *d) {
    if (!d) { errno = EBADF; return (struct dirent *)0; }
    if (d->pos >= d->n) {
        /* End of directory: NULL and errno UNTOUCHED. That's how a caller
         * distinguishes EOF from a real failure, so do not set errno here. */
        return (struct dirent *)0;
    }
    return &d->ents[d->pos++];
}

/* Replays the snapshot from the top. POSIX says rewinddir() re-reads the
 * directory; ours re-reads what opendir() captured. Anything needing to observe
 * concurrent changes must opendir() again. */
void rewinddir(DIR *d) {
    if (d) d->pos = 0;
}

int closedir(DIR *d) {
    if (!d) { errno = EBADF; return -1; }
    free(d->ents);
    free(d);
    return 0;
}

/* Our DIR is a SNAPSHOT taken by opendir(), not an open handle: the kernel's
 * readdir is path-based, so there is no descriptor behind it to hand back. Fail
 * rather than invent one -- a caller given a bogus fd would openat()/fstat() the
 * wrong object. This is also why fdopendir() is left undefined. */
int dirfd(DIR *d) {
    (void)d;
    errno = ENOTSUP;
    return -1;
}

/* ------------------------------------------------------------------ */
/* Scheduling                                                          */
/* ------------------------------------------------------------------ */

/* Declared here, not by <sched.h>: newlib gates sched_yield behind
 * _POSIX_THREADS || _POSIX_PRIORITY_SCHEDULING, and EmbLink satisfies NEITHER
 * (no pthread library; no priority-scheduling API). Defining one of those macros
 * to reveal this single prototype would also declare sched_setparam/
 * sched_getscheduler/sched_rr_get_interval/... which we do not implement.
 * (CPython gets its own declaration because its pthread_stubs.h defines
 * _POSIX_THREADS.) */
int sched_yield(void);
int sched_yield(void) {
    embk_syscall0(EMBK_SYS_yield);
    return 0;   /* the kernel's yield has no failure mode */
}

int usleep(useconds_t useconds) {
    if (useconds == 0) {           /* "give up the CPU now" */
        embk_syscall0(EMBK_SYS_yield);
        return 0;
    }
    /* EmbLink's sleep is millisecond-granular (SYS_sleep_ms, HPET-paced), so
     * round UP: waking EARLY than asked breaks pacing loops, whereas a fraction
     * of a millisecond extra is harmless. Sub-millisecond sleeps therefore cost
     * a full tick -- honest, and callers who need finer waits have none here. */
    uint64_t ms = ((uint64_t)useconds + 999u) / 1000u;
    embk_syscall1(EMBK_SYS_sleep_ms, (int64_t)ms);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Filesystem                                                          */
/* ------------------------------------------------------------------ */

int mkdir(const char *path, mode_t mode) {
    (void)mode;   /* EMBKFS has no permission bits -- see umask() below */
    char abs[EMBK_PATH_MAX];
    if (path_abs(path, abs, sizeof abs) != 0) return -1;   /* errno set */

    int64_t ret = embk_syscall1(EMBK_SYS_mkdir, (int64_t)(intptr_t)abs);
    if (embk_is_err(ret)) return embk_fail(ret);
    return 0;
}

int rmdir(const char *path) {
    char abs[EMBK_PATH_MAX];
    if (path_abs(path, abs, sizeof abs) != 0) return -1;   /* errno set */

    int64_t ret = embk_syscall1(EMBK_SYS_rmdir, (int64_t)(intptr_t)abs);
    if (embk_is_err(ret)) return embk_fail(ret);
    return 0;
}

/* access() collapses to "does it exist?" because EmbLink has no uids and EMBKFS
 * has no permission bits: R_OK/W_OK/X_OK are granted for anything stat() can
 * see. That is not a shortcut -- with no access control there is nothing that
 * could deny the request. If permissions ever land, this MUST start consulting
 * them instead of ignoring `mode`. */
int access(const char *path, int mode) {
    (void)mode;
    struct stat st;
    return stat(path, &st);   /* sets errno (ENOENT/ENOTDIR/...) on failure */
}

/* ------------------------------------------------------------------ */
/* Working directory                                                   */
/* ------------------------------------------------------------------ */

/* getcwd/chdir are REAL now: see g_cwd + path_abs above. The cwd is this
 * process's own libc state -- per-process by construction, never shared, never
 * inherited unless a parent names PWD at spawn. */
char *getcwd(char *buf, size_t size) {
    if (!buf) {
        /* The GNU "allocate it for me" extension is deliberately not offered;
         * callers passing NULL expect malloc'd memory with different ownership
         * rules, and quietly guessing would leak or double-free. */
        errno = EINVAL;
        return (char *)0;
    }
    size_t need = strlen(g_cwd) + 1;
    if (size < need) { errno = ERANGE; return (char *)0; }
    memcpy(buf, g_cwd, need);
    return buf;
}

/* chdir VERIFIES before it moves: the target must exist and be a directory.
 * Storing an unchecked string would let every later relative path resolve
 * against somewhere that isn't there, turning one bad chdir into a pile of
 * confusing ENOENTs far from the cause. */
int chdir(const char *path) {
    char abs[EMBK_PATH_MAX];
    if (path_abs(path, abs, sizeof abs) != 0) return -1;   /* errno set */

    struct stat st;
    if (stat(abs, &st) != 0) return -1;                     /* ENOENT etc */
    if (!S_ISDIR(st.st_mode)) { errno = ENOTDIR; return -1; }

    memcpy(g_cwd, abs, strlen(abs) + 1);
    return 0;
}

/* Called by crt0 BEFORE ctors and main, once environ is installed.
 *
 * PWD is how a parent HANDS OVER a starting directory -- the same explicit
 * naming as envp and file-actions, using the mechanism that already exists. No
 * PWD means no inheritance, and we stay at "/", which is the honest default.
 *
 * Best-effort on purpose: a PWD naming somewhere gone must not kill startup, it
 * must leave us at the root. chdir() does the verifying. */
void embk_cwd_init_from_env(void) {
    const char *pwd = getenv("PWD");
    if (pwd && pwd[0] == '/') (void)chdir(pwd);
}

int fchdir(int fd) {
    /* There IS a cwd now, but an fd does not know its own path: fd_entry holds
     * a vnode (mnt + ino), and nothing maps an ino back to a name. Answering
     * would mean inventing a path. Honest ENOSYS until the VFS can name a
     * vnode. */
    (void)fd;
    errno = ENOSYS;
    return -1;
}

/* ------------------------------------------------------------------ */
/* System parameters                                                   */
/* ------------------------------------------------------------------ */

int getpagesize(void) {
    return 4096;   /* kernel/mm/pmm.h PAGE_SIZE -- the real VMM page size */
}

long sysconf(int name) {
    switch (name) {
    case _SC_PAGESIZE:                    /* == _SC_PAGE_SIZE */
        return 4096;
    case _SC_OPEN_MAX:
        return 64;                        /* kernel/fs/fd.h FD_MAX_OPEN */
    default:
        /* Everything else -- including _SC_NPROCESSORS_ONLN and _SC_CLK_TCK --
         * is genuinely unknown to us: there is no syscall to ask the kernel its
         * CPU count, and times() does not advance. Returning a plausible 1 would
         * be a fabricated answer callers cannot tell from a measured one; -1 is
         * the POSIX "no limit / not supported" reply and os.cpu_count() simply
         * reports None. */
        errno = EINVAL;
        return -1;
    }
}

/* POSIX: umask() cannot fail and returns the PREVIOUS mask. EMBKFS has no
 * permission bits for a mask to filter, so the value is inert -- we report 0
 * (nothing masked) and ignore the new one. Note this is the ONE call here that
 * must not fail, which is why it doesn't. */
mode_t umask(mode_t mask) {
    (void)mask;
    return (mode_t)0;
}

/* ------------------------------------------------------------------ */
/* Genuinely absent -- fail rather than pretend                        */
/* ------------------------------------------------------------------ */

/* fsync/fdatasync: refusing is the SAFE direction. Returning 0 would promise
 * durability we have not verified EMBKFS provides, and a caller told "your data
 * is on disk" cannot detect the lie -- that is precisely how data is lost. If
 * the block layer is ever confirmed to commit synchronously (or a real flush
 * syscall lands), these become 0 / a real flush. */
int fsync(int fd) {
    (void)fd;
    errno = ENOSYS;
    return -1;
}

int fdatasync(int fd) {
    (void)fd;
    errno = ENOSYS;
    return -1;
}

/* No dup: the fd table has no duplicate-into-lowest-free operation, and faking
 * one by re-opening would give a SEPARATE file position, which is the opposite
 * of what dup() promises (a shared cursor). */
int dup(int fd) {
    (void)fd;
    errno = ENOSYS;
    return -1;
}

/* There is no fcntl SYSCALL, but the descriptor-flag commands are answerable
 * from what EmbLink's process model already guarantees, and refusing them is
 * NOT the conservative choice -- it broke CPython outright:
 *
 *     _Py_wfopen() -> fopen() succeeds -> make_non_inheritable(fileno(f))
 *       -> fcntl(fd, F_GETFD) -> our ENOSYS -> fclose() -> NULL
 *
 * so EVERY file CPython opens through that path failed. It cost a long hunt
 * because getpath.py swallows the OSError, leaving only "No module named
 * 'encodings'" as the symptom.
 *
 * F_GETFD/F_SETFD (FD_CLOEXEC) -- ANSWERED, and honestly. FD_CLOEXEC promises
 * "this descriptor will not survive into an exec'd image". EmbLink HAS NO EXEC:
 * the model is spawn() + explicit file-actions, so a child only ever receives
 * descriptors the parent names deliberately, and nothing is implicitly
 * inherited. That promise therefore already holds for every fd, unconditionally
 * -- reporting FD_CLOEXEC set is a true statement about this OS, not a
 * convenient stub. (Should exec ever exist -- it will not, by design -- this
 * must become real state.)
 *
 * F_GETFL/F_SETFL -- still REFUSED. O_NONBLOCK genuinely cannot be honoured,
 * and here a false success is exactly the trap: a caller told non-blocking mode
 * was set would then hang forever in a blocking read. F_DUPFD likewise, since
 * dup() itself is ENOSYS (see above).
 */
/* uname(): real facts about this OS -- nothing here is faked or guessed.
 * nodename is the sysname: one machine, no network identity to distinguish. */
#include <sys/utsname.h>
int uname(struct utsname *buf) {
    if (!buf) { errno = EFAULT; return -1; }
    strcpy(buf->sysname,  "EmbLink");
    strcpy(buf->nodename, "emblink");
    strcpy(buf->release,  "1.0");
    strcpy(buf->version,  "EmbLinkOS");
    strcpy(buf->machine,  "x86_64");
    return 0;
}

int fcntl(int fd, int cmd, ...) {
    /* Validate the fd for every command: a bad fd must be EBADF, not a
     * cheerful answer about a descriptor that isn't open. */
    struct stat st;
    if (fstat(fd, &st) != 0) {
        errno = EBADF;
        return -1;
    }

    switch (cmd) {
    case F_GETFD:
        /* Every fd is effectively close-on-exec here -- see above. */
        return FD_CLOEXEC;
    case F_SETFD:
        /* Accept whatever is asked: with no exec, both "close on exec" and
         * "stay open across exec" describe the same (unobservable) reality, so
         * there is nothing to store and nothing that could later disagree. */
        return 0;
    default:
        errno = ENOSYS;
        return -1;
    }
}

/* EmbLink's process model is spawn()+file-actions by design -- there is no
 * fork/exec pair and there will not be one. execv() cannot be emulated: it must
 * REPLACE the running image, which spawn (a new process) does not do. */
int execv(const char *path, char *const argv[]) {
    (void)path; (void)argv;
    errno = ENOSYS;
    return -1;
}

int symlink(const char *target, const char *linkpath) {
    /* EMBKFS can STORE a link type (VFS_DT_LNK) but nothing creates or follows
     * one -- see lstat() above. Creating a link nothing can traverse would be
     * worse than refusing. */
    (void)target; (void)linkpath;
    errno = ENOSYS;
    return -1;
}

int chroot(const char *path) {
    (void)path;
    errno = ENOSYS;   /* no per-process root; a no-op "success" would be a
                       * security claim we cannot back */
    return -1;
}

int setgroups(int ngroups, const gid_t *grouplist) {
    (void)ngroups; (void)grouplist;
    errno = ENOSYS;   /* no uids/gids at all -- same reasoning as chroot */
    return -1;
}

int pause(void) {
    /* pause() waits for a signal. With no signal delivery it could only block
     * forever, which is a hang dressed up as an API. */
    errno = ENOSYS;
    return -1;
}

/* No poll/select machinery: fds have no readiness notification, only blocking
 * reads. Returning 0 ("nothing ready, timed out") would turn every select loop
 * into a silent spin. */
int select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
           struct timeval *timeout) {
    (void)nfds; (void)readfds; (void)writefds; (void)exceptfds; (void)timeout;
    errno = ENOSYS;
    return -1;
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
/* Entropy                                                             */
/* ------------------------------------------------------------------ */

/* getentropy(buf, len) -- newlib's arc4random() and libstdc++'s
 * std::random_device both reference this, so a STATIC link drags it in the
 * moment a C++ program touches <iostream>/<random> (whole-archive-member
 * granularity), whether or not the program wants randomness.
 *
 * Implemented on RDRAND: the CPU's on-die entropy source, an UNPRIVILEGED
 * instruction, so this needs no syscall at all.
 *
 * DELIBERATELY FAILS (ENOSYS) rather than substituting uptime/pid mixing when
 * RDRAND is absent. getentropy is a SECURITY primitive -- its whole contract
 * is unpredictable bytes. A plausible-looking fallback built from a clock is
 * trivially guessable, and would silently seed anything that later does
 * crypto with attacker-predictable state. A caller that gets -1 can decide;
 * one that gets fake entropy cannot. If a soft RNG is ever wanted, it must be
 * a differently-named function nobody mistakes for this one. */
static int cpu_has_rdrand(void) {
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(1u), "c"(0u));
    return (ecx >> 30) & 1u;    /* CPUID.01H:ECX.RDRAND[bit 30] */
}

static int rdrand64(uint64_t *out) {
    /* RDRAND sets CF=1 on success. It may legitimately fail transiently when
     * the on-die pool is momentarily drained; Intel's guidance is a bounded
     * retry, NOT an infinite spin. */
    for (int attempt = 0; attempt < 10; attempt++) {
        unsigned char ok = 0;
        __asm__ volatile("rdrand %0; setc %1"
                         : "=r"(*out), "=qm"(ok) :: "cc");
        if (ok) return 1;
    }
    return 0;
}

int getentropy(void *buf, size_t len) {
    if (len > 256) {          /* the documented cap; POSIX/OpenBSD agree */
        errno = EIO;
        return -1;
    }
    if (!buf && len) {
        errno = EFAULT;
        return -1;
    }
    if (!cpu_has_rdrand()) {
        errno = ENOSYS;       /* honest: we have no entropy source */
        return -1;
    }

    unsigned char *p = (unsigned char *)buf;
    size_t done = 0;
    while (done < len) {
        uint64_t r;
        if (!rdrand64(&r)) {  /* hardware wouldn't produce: say so */
            errno = EIO;
            return -1;
        }
        size_t n = len - done;
        if (n > sizeof r) n = sizeof r;
        memcpy(p + done, &r, n);
        done += n;
    }
    return 0;
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

/* clock_gettime() over the two real clocks this OS actually has:
 *
 *   CLOCK_REALTIME  -> the RTC (SYS_gettimeofday). ONE-SECOND granularity: the
 *                      RTC is a seconds counter and reports tv_usec == 0. We do
 *                      not blend in the HPET to manufacture sub-second digits;
 *                      that would invent precision the wall clock doesn't have.
 *   CLOCK_MONOTONIC -> the HPET uptime counter (SYS_uptime_ms), millisecond
 *                      granularity. This is what a monotonic clock is for: it
 *                      counts from boot and never jumps when the wall clock is
 *                      set. MONOTONIC_RAW/BOOTTIME are the same source -- we
 *                      have no clock slewing, and uptime does span suspend.
 *
 * The CPU-time clocks are deliberately NOT served: there is still no
 * per-process CPU accounting (see times() below), so answering them would hand
 * back a number the caller cannot tell is fiction. EINVAL is the honest reply.
 */
int clock_gettime(clockid_t clock_id, struct timespec *tp) {
    if (!tp) { errno = EFAULT; return -1; }

    switch ((int)clock_id) {
    case CLOCK_REALTIME: {
        struct timeval tv;
        if (gettimeofday(&tv, (void *)0) != 0) return -1;   /* errno already set */
        tp->tv_sec  = tv.tv_sec;
        tp->tv_nsec = (long)tv.tv_usec * 1000L;
        return 0;
    }
    case CLOCK_MONOTONIC:
#ifdef CLOCK_MONOTONIC_RAW
    case CLOCK_MONOTONIC_RAW:
#endif
#ifdef CLOCK_BOOTTIME
    case CLOCK_BOOTTIME:
#endif
    {
        uint64_t ms = (uint64_t)embk_syscall0(EMBK_SYS_uptime_ms);
        tp->tv_sec  = (time_t)(ms / 1000ULL);
        tp->tv_nsec = (long)((ms % 1000ULL) * 1000000ULL);
        return 0;
    }
    default:
        errno = EINVAL;
        return -1;
    }
}

/* Report the granularity we genuinely deliver, not the granularity of the
 * struct: callers that pace work off clock_getres() would busy-spin if we
 * claimed 1ns. */
int clock_getres(clockid_t clock_id, struct timespec *res) {
    long ns;
    switch ((int)clock_id) {
    case CLOCK_REALTIME:      ns = 1000000000L; break;  /* RTC ticks once a second */
    case CLOCK_MONOTONIC:
#ifdef CLOCK_MONOTONIC_RAW
    case CLOCK_MONOTONIC_RAW:
#endif
#ifdef CLOCK_BOOTTIME
    case CLOCK_BOOTTIME:
#endif
                              ns = 1000000L;    break;  /* HPET exposed as ms */
    default: errno = EINVAL; return -1;
    }
    if (res) { res->tv_sec = 0; res->tv_nsec = ns; }
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
    /* REAL since SYS_unlink (53) landed with the shell's rm -- this stub's old
     * "no unlink op yet" claim went stale without anyone noticing, because the
     * shell calls embk_unlink directly and nothing else deleted files. git
     * deletes files constantly (lockfiles, tmp objects). */
    char abs[EMBK_PATH_MAX];
    if (path_abs(path, abs, sizeof abs) != 0) return -1;
    int64_t ret = embk_syscall1(EMBK_SYS_unlink, (int64_t)(intptr_t)abs);
    if (embk_is_err(ret)) return embk_fail(ret);
    return 0;
}

/* ======================================================================== */
/* The git-port surface (2026-07-17). Everything git links that newlib does */
/* not define. Three honest categories, and the category is the point:     */
/*                                                                         */
/*   REAL     -- the capability exists; implement it truthfully.           */
/*   VACUOUS  -- the promise already holds on EmbLink whatever we do;      */
/*               saying "yes" is a true statement (the FD_CLOEXEC rule).   */
/*   ABSENT   -- the capability genuinely does not exist here; refuse      */
/*               loudly (ENOSYS et al), never fake success.                */
/* ======================================================================== */

#include <stdio.h>
#include <sys/utsname.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/statvfs.h>
#include <sys/mman.h>
#include <pwd.h>

/* ---- REAL ------------------------------------------------------------- */

/* rename: a thin pass-through, because the KERNEL does the real thing --
 * replacing an existing destination inside the SAME commit that moves the name
 * (embkfs_rename). This used to unlink the destination first and rename after,
 * which is not atomic: a crash between the two lost the destination while the
 * source survived. git's whole lockfile protocol is built on rename being
 * atomic, so faking it here was borrowing against exactly the guarantee the
 * caller was relying on. */
int rename(const char *oldp, const char *newp) {
    char oa[EMBK_PATH_MAX], na[EMBK_PATH_MAX];
    if (path_abs(oldp, oa, sizeof oa) != 0) return -1;
    if (path_abs(newp, na, sizeof na) != 0) return -1;
    int64_t ret = embk_syscall2(EMBK_SYS_rename,
                                (int64_t)(intptr_t)oa, (int64_t)(intptr_t)na);
    if (embk_is_err(ret)) return embk_fail(ret);
    return 0;
}

int ftruncate(int fd, off_t length) {
    if (length < 0) { errno = EINVAL; return -1; }
    int64_t ret = embk_syscall2(EMBK_SYS_ftruncate, fd, (int64_t)length);
    if (embk_is_err(ret)) return embk_fail(ret);
    return 0;
}

unsigned sleep(unsigned seconds) {
    embk_syscall1(EMBK_SYS_sleep_ms, (int64_t)seconds * 1000);
    return 0;   /* nothing interrupts a sleep here, so the remainder is 0 */
}

int gethostname(char *name, size_t len) {
    /* The same truth uname() tells: one machine, no network identity. */
    const char *hn = "emblink";
    size_t need = strlen(hn) + 1;
    if (!name) { errno = EFAULT; return -1; }
    if (len < need) { errno = ENAMETOOLONG; return -1; }
    memcpy(name, hn, need);
    return 0;
}

/* sigaction over signal(): the REGISTRATION half is real (newlib dispatches
 * self-signals synchronously -- see memory's signals notes); there is no async
 * delivery for the flags to modify, so sa_flags (incl. our SA_RESTART=0) has
 * nothing to do and sa_mask is meaningless (see sigprocmask below). */
int sigaction(int sig, const struct sigaction *act, struct sigaction *oldact) {
    if (act) {
        void (*prev)(int) = signal(sig, act->sa_handler);
        if (prev == SIG_ERR) return -1;   /* signal() set errno */
        if (oldact) {
            memset(oldact, 0, sizeof *oldact);
            oldact->sa_handler = prev;
        }
    } else if (oldact) {
        /* Query without change: set-and-restore. No race -- signal state is
         * per-process and nothing asynchronous can observe the window. */
        void (*cur)(int) = signal(sig, SIG_DFL);
        if (cur == SIG_ERR) return -1;
        signal(sig, cur);
        memset(oldact, 0, sizeof *oldact);
        oldact->sa_handler = cur;
    }
    return 0;
}

/* Pure text, nothing network about it. Static buffer per the historical API. */
char *inet_ntoa(struct in_addr in) {
    static char buf[16];
    uint32_t a = ntohl(in.s_addr);
    snprintf(buf, sizeof buf, "%u.%u.%u.%u",
             (a >> 24) & 0xff, (a >> 16) & 0xff, (a >> 8) & 0xff, a & 0xff);
    return buf;
}

int h_errno;   /* the legacy resolver errno; only ever set by our refusals */
const char *hstrerror(int err) {
    (void)err;
    return "name resolution unavailable (EmbLink has no network stack)";
}

/* ---- VACUOUS ----------------------------------------------------------- */

/* No asynchronous signal delivery exists, so every mask is already in effect:
 * nothing can arrive whether or not it is "blocked". Reporting the empty mask
 * and succeeding is a true statement, same shape as FD_CLOEXEC. */
int sigprocmask(int how, const sigset_t *set, sigset_t *oldset) {
    (void)how; (void)set;
    if (oldset) *oldset = 0;
    return 0;
}

/* ---- ABSENT (refuse loudly; never fake success) ------------------------ */

/* alarm() cannot report failure (returns the previous remainder), so this is
 * the one stub that cannot refuse honestly through its return value. Nothing
 * will ever fire -- there is no async delivery. Callers use it as a watchdog;
 * on EmbLink the watched operation simply is not watched. */
unsigned alarm(unsigned seconds) {
    (void)seconds;
    return 0;
}

/* REAL since SYS_chmod (64): EMBKFS inodes always carried a mode, only the road
 * from userspace was missing. The kernel preserves the file-type bits. */
int chmod(const char *path, mode_t mode) {
    char abs[EMBK_PATH_MAX];
    if (path_abs(path, abs, sizeof abs) != 0) return -1;
    int64_t ret = embk_syscall2(EMBK_SYS_chmod, (int64_t)(intptr_t)abs, (int64_t)mode);
    if (embk_is_err(ret)) return embk_fail(ret);
    return 0;
}

ssize_t readlink(const char *path, char *buf, size_t bufsiz) {
    /* The kernel HAS embkfs_readlink_object; no syscall exposes it yet. git
     * only readlinks entries lstat reported as symlinks, which EmbLink repos
     * will not contain (symlink() below is ENOSYS, so git init sets
     * core.symlinks=false and never creates one). */
    (void)path; (void)buf; (void)bufsiz;
    errno = ENOSYS;
    return -1;
}

int pipe(int fds[2]) {
    /* EmbLink HAS pipes (the shell's pipelines run on them) -- what is missing
     * is only this POSIX veneer, which needs a lowest-free-fd install the SDK
     * does not expose yet. ENOSYS is literally true ("not implemented"), and
     * every git caller sits behind a fork/exec that refuses first. */
    (void)fds;
    errno = ENOSYS;
    return -1;
}

/* Process control that presumes fork/exec/sessions -- the model this OS
 * deliberately does not have (spawn + explicit handles instead). */
pid_t waitpid(pid_t pid, int *status, int options) {
    (void)pid; (void)status; (void)options;
    errno = ENOSYS; return (pid_t)-1;
}
/* WEAK: a libc stub must YIELD to an application that ships its own. CPython
 * carries a dup2 compat implementation (for platforms lacking the call) and a
 * strong definition here collides with it -- "multiple definition of `dup2'",
 * found the moment python.elf was relinked against this libc. Weak means their
 * definition simply wins, which is what a portable program expects from a libc
 * it is overriding. (Theirs fails too -- it goes through fcntl(F_DUPFD), which
 * we also refuse -- but it fails as THEIR code, on their terms.) */
__attribute__((weak))
int dup2(int oldfd, int newfd) { (void)oldfd; (void)newfd; errno = ENOSYS; return -1; }
pid_t setsid(void)             { errno = ENOSYS; return (pid_t)-1; }
pid_t getpgid(pid_t pid)       { (void)pid; errno = ENOSYS; return (pid_t)-1; }
pid_t tcgetpgrp(int fd)        { (void)fd; errno = ENOSYS; return (pid_t)-1; }
pid_t getppid(void) {
    /* The kernel tracks parent pids but no syscall exposes them, and parental
     * authority here is HANDLES, not pid arithmetic. 0 = "not visible". */
    return 0;
}

int execl(const char *path, const char *arg, ...)  { (void)path; (void)arg; errno = ENOSYS; return -1; }
int execlp(const char *file, const char *arg, ...) { (void)file; (void)arg; errno = ENOSYS; return -1; }
int execvp(const char *file, char *const argv[])   { (void)file; (void)argv; errno = ENOSYS; return -1; }

/* Identity: one user, whole-machine authority. 0 states that plainly. There
 * is NO user database, so the getpw* lookups honestly find nobody -- git then
 * requires ident from config/env (user.name, user.email), which is the
 * supported EmbLink way. */
uid_t getuid(void)  { return 0; }
uid_t geteuid(void) { return 0; }
struct passwd *getpwuid(uid_t uid)        { (void)uid;  errno = ENOENT; return NULL; }
struct passwd *getpwnam(const char *name) { (void)name; errno = ENOENT; return NULL; }

char *getpass(const char *prompt) { (void)prompt; errno = ENOTTY; return NULL; }
int tcgetattr(int fd, struct termios *t) { (void)fd; (void)t; errno = ENOTTY; return -1; }
int tcsetattr(int fd, int actions, const struct termios *t) {
    (void)fd; (void)actions; (void)t; errno = ENOTTY; return -1;
}

int ioctl(int fd, unsigned long request, ...) {
    /* TIOCGWINSZ is the caller that matters; the console has no winsize
     * protocol, and a fabricated 80x24 would be a guess dressed as a
     * measurement. Callers fall back to their defaults. */
    (void)fd; (void)request;
    errno = ENOSYS;
    return -1;
}

/* NO mmap. Memory here is sbrk (a real growable heap) and typed objects
 * (shared surfaces, zero-copy windows); mapping a FILE into an address space is
 * a capability this OS has never had. Refused, not faked out of malloc: an
 * anonymous-looking block would silently not be MAP_SHARED and would not be
 * backed by the file the caller named. TCC's `-run` is the caller; `tcc -o`
 * never reaches here. */
void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off) {
    (void)addr; (void)len; (void)prot; (void)flags; (void)fd; (void)off;
    errno = ENOSYS;
    return MAP_FAILED;
}
int munmap(void *addr, size_t len) { (void)addr; (void)len; errno = ENOSYS; return -1; }
int mprotect(void *addr, size_t len, int prot) {
    (void)addr; (void)len; (void)prot; errno = ENOSYS; return -1;
}
int msync(void *addr, size_t len, int flags) {
    (void)addr; (void)len; (void)flags; errno = ENOSYS; return -1;
}

int statvfs(const char *path, struct statvfs *buf)  { (void)path; (void)buf; errno = ENOSYS; return -1; }
int fstatvfs(int fd, struct statvfs *buf)           { (void)fd;   (void)buf; errno = ENOSYS; return -1; }

/* No network stack -- not unconfigured, ABSENT. */
int socket(int d, int t, int p)  { (void)d; (void)t; (void)p; errno = ENOSYS; return -1; }
int connect(int fd, const struct sockaddr *a, socklen_t l) { (void)fd; (void)a; (void)l; errno = ENOSYS; return -1; }
int shutdown(int fd, int how)    { (void)fd; (void)how; errno = ENOSYS; return -1; }
int setsockopt(int fd, int lvl, int opt, const void *v, socklen_t l) {
    (void)fd; (void)lvl; (void)opt; (void)v; (void)l; errno = ENOSYS; return -1;
}
ssize_t recv(int fd, void *buf, size_t len, int flags) {
    (void)fd; (void)buf; (void)len; (void)flags; errno = ENOSYS; return -1;
}
struct hostent *gethostbyname(const char *name) {
    (void)name;
    h_errno = NO_RECOVERY;
    return NULL;
}
struct servent *getservbyname(const char *name, const char *proto) {
    (void)name; (void)proto;
    return NULL;
}
