#include "arch/x86_64/syscall/syscall.h"
#include "arch/x86_64/irq/idt.h"
#include "arch/x86_64/cpu/kcontext.h"
#include "arch/x86_64/syscall/usercopy.h"
#include "drivers/char/serial.h"
#include "include/kprintf.h"   /* sys_read's copy_to_user fault-path diagnostic */
#include "drivers/timer/rtc.h"
#include "drivers/timer/hpet.h"
#include "drivers/timer/timer.h"
#include "include/errno.h"
#include "include/types.h"
#include "include/kstring.h"
#include "process/process.h"
#include "gfx/surface.h"
#include "gfx/compositor.h"
#include "drivers/video/framebuffer.h"
#include "drivers/input/mouse.h"
#include "drivers/input/keyboard.h"
#include "include/kmalloc.h"
#include "ipc/channel.h"
#include "ipc/endpoint.h"
#include "fs/fd.h"
#include "fs/vfs.h"
#include <stdint.h>

// Bounce-buffer chunk size shared by every syscall that streams a user
// buffer through a bounded kernel stack buffer (write/read). Arbitrary but
// small enough to be cheap on the kernel stack.
#define SYSCALL_IO_CHUNK 256
// Max path length copy_string_from_user() will accept for open()/stat().
#define SYSCALL_PATH_MAX 256

/* Saved kernel context to resume on exit(), defined in usermode.c. */
extern struct kcontext g_user_exit_ctx;



/* --- Individual syscall handlers --- */

/* write(fd, buf, len) -> bytes written. fd==1 (stdout) goes to the serial
 * console directly, since there's no real "console" vnode to open yet; any
 * other fd routes through the fd/VFS layer (vfs_fd_write), which already
 * fully supports EMBKFS/FAT32.
 *
 * `buf` is a USER pointer: copied through copy_from_user() in bounded
 * chunks rather than dereferenced directly, so a ring-3 caller can't hand
 * the kernel a kernel-space (or unmapped) address and get it read back —
 * see docs/TODO.md's "Security — user-pointer validation" entry, now closed. */
static int64_t sys_write(struct regs *r) {
    int fd = (int)r->rdi;
    const char *buf = (const char *)r->rsi;
    size_t len = (size_t)r->rdx;

    char chunk[SYSCALL_IO_CHUNK];
    size_t done = 0;
    while (done < len) {
        size_t n = len - done;
        if (n > sizeof(chunk)) {
            n = sizeof(chunk);
        }
        if (copy_from_user(chunk, buf + done, n) != EMBK_OK) {
            return done > 0 ? (int64_t)done : -EMBK_EFAULT;
        }

        size_t advanced;
        size_t written = 0;
        int rc = vfs_fd_write(fd, chunk, n, &written);
        if (rc != EMBK_OK) {
            return done > 0 ? (int64_t)done : rc;
        }
        advanced = written;

        done += advanced;
        if (advanced < n) {
            break;   // short write (fd path only); stop rather than looping forever
        }
    }
    return (int64_t)done;
}

/* read(fd, buf, len) -> bytes read, via the fd/VFS layer. No fd 0 (stdin)
 * wiring yet — there's no "console input" vnode, only the raw keyboard
 * buffer main.c's loop drains directly. `buf` is a USER pointer: filled
 * through a kernel bounce buffer + copy_to_user(), the mirror image of
 * sys_write's copy_from_user() pattern. */
static int64_t sys_read(struct regs *r) {
    int fd = (int)r->rdi;
    char *buf = (char *)r->rsi;
    size_t len = (size_t)r->rdx;

    if ( fd == 1 || fd == 2) {
        return -EMBK_EINVAL;   // no stdio-fd read support yet
    }

    char chunk[SYSCALL_IO_CHUNK];
    size_t done = 0;
    while (done < len) {
        size_t n = len - done;
        if (n > sizeof(chunk)) {
            n = sizeof(chunk);
        }
        size_t got = 0;
        int rc = vfs_fd_read(fd, chunk, n, &got);
        if (rc != EMBK_OK) {
            return done > 0 ? (int64_t)done : rc;
        }
        if (got > 0 && copy_to_user(buf + done, chunk, got) != EMBK_OK) {
            /* The fs already consumed `got` bytes (vfs_fd_read advanced the
             * position) but the caller never received them. Returning here
             * WITHOUT rewinding silently dropped this chunk -- the next read
             * continued past it (observed: font.ttf arriving k*256 bytes
             * short with varying content under -smp 4, -> textless UI).
             * Rewind so a retrying caller sees every byte exactly once. */
            uint64_t np = 0;
            vfs_fd_seek(fd, -(int64_t)got, 1 /* SEEK_CUR */, &np);
            kprintf("sys_read: copy_to_user FAULT pid=%u dst=0x%llx off_done=%llu (rewound %llu)\n",
                    current_thread ? (unsigned)current_process->pid : 0u,
                    (unsigned long long)(uintptr_t)(buf + done),
                    (unsigned long long)done, (unsigned long long)got);
            return done > 0 ? (int64_t)done : -EMBK_EFAULT;
        }
        done += got;
        if (got < n) {
            break;   // short read: EOF (or similar) -- stop here
        }
    }
    return (int64_t)done;
}

/* open(path, flags, mode) -> fd, or -errno. `path` is copied in through a
 * bounded kernel buffer (copy_string_from_user) rather than resolved
 * directly from the user pointer. */
static int64_t sys_open(struct regs *r) {
    const char *user_path = (const char *)r->rdi;
    int flags = (int)r->rsi;
    uint32_t mode = (uint32_t)r->rdx;

    char path[SYSCALL_PATH_MAX];
    int len = copy_string_from_user(path, user_path, sizeof(path));
    if (len < 0) {
        return len;   // -EMBK_EFAULT or -EMBK_ENAMETOOLONG
    }

    return vfs_open(path, flags, mode);
}

/* close(fd) -> 0, or -errno. */
static int64_t sys_close(struct regs *r) {
    int fd = (int)r->rdi;
    return vfs_close(fd);
}

/* lseek(fd, offset, whence) -> new absolute offset, or -errno.
 * whence: 0 = SEEK_SET, 1 = SEEK_CUR, 2 = SEEK_END (vfs_fd_seek's existing
 * numeric convention -- no libc yet to define the usual named constants
 * against). */
static int64_t sys_lseek(struct regs *r) {
    int fd = (int)r->rdi;
    int64_t offset = (int64_t)r->rsi;
    int whence = (int)r->rdx;

    uint64_t new_pos = 0;
    int rc = vfs_fd_seek(fd, offset, whence, &new_pos);
    if (rc != EMBK_OK) {
        return rc;
    }
    return (int64_t)new_pos;
}

/* stat(path, out_stat) -> 0, or -errno. `out_stat` must point at
 * sizeof(struct vfs_stat) bytes of user memory; filled via copy_to_user(). */
static int64_t sys_stat(struct regs *r) {
    const char *user_path = (const char *)r->rdi;
    struct vfs_stat *user_out = (struct vfs_stat *)r->rsi;

    char path[SYSCALL_PATH_MAX];
    int len = copy_string_from_user(path, user_path, sizeof(path));
    if (len < 0) {
        return len;
    }

    struct vfs_stat st;
    int rc = vfs_stat(path, &st);
    if (rc != EMBK_OK) {
        return rc;
    }

    if (copy_to_user(user_out, &st, sizeof(st)) != EMBK_OK) {
        return -EMBK_EFAULT;
    }
    return 0;
}

/* readdir(path, out_entries, max_entries) -> number of entries written, or
 * -errno. Not POSIX's getdents (variable-length, fd-based, resumable) --
 * this kernel has no libc yet to match an ABI against, so it's a single
 * call that walks the whole directory (path-based, like every other vfs_*
 * call here) and packs up to max_entries fixed-size records into the
 * caller's buffer. Revisit once something needs a directory too large to
 * read in one call. */
struct sys_dirent {
    uint64_t ino;
    uint8_t  type;      // VFS_DT_*
    char     name[59];  // NUL-terminated, truncated if longer (sizeof == 64 with ino+type)
};

struct sys_readdir_ctx {
    struct sys_dirent *user_entries;
    uint32_t max_entries;
    uint32_t count;
    int      error;
};

static int sys_readdir_cb(const char *name, uint8_t name_len, uint8_t type,
                          uint64_t ino, void *ctx_) {
    struct sys_readdir_ctx *ctx = (struct sys_readdir_ctx *)ctx_;
    if (ctx->count >= ctx->max_entries) {
        return EMBK_OK;   // caller's buffer is full; stop collecting, not an error
    }

    struct sys_dirent ent;
    memset(&ent, 0, sizeof(ent));
    ent.ino = ino;
    ent.type = type;
    size_t copy_len = name_len < sizeof(ent.name) - 1 ? name_len : sizeof(ent.name) - 1;
    memcpy(ent.name, name, copy_len);
    ent.name[copy_len] = '\0';

    if (copy_to_user(&ctx->user_entries[ctx->count], &ent, sizeof(ent)) != EMBK_OK) {
        ctx->error = -EMBK_EFAULT;
        return -EMBK_EFAULT;   // stop the walk
    }
    ctx->count++;
    return EMBK_OK;
}

static int64_t sys_readdir(struct regs *r) {
    const char *user_path = (const char *)r->rdi;
    struct sys_dirent *user_entries = (struct sys_dirent *)r->rsi;
    uint32_t max_entries = (uint32_t)r->rdx;

    char path[SYSCALL_PATH_MAX];
    int len = copy_string_from_user(path, user_path, sizeof(path));
    if (len < 0) {
        return len;
    }

    struct sys_readdir_ctx ctx = {
        .user_entries = user_entries,
        .max_entries = max_entries,
        .count = 0,
        .error = 0,
    };
    int rc = vfs_readdir(path, sys_readdir_cb, &ctx);
    if (ctx.error != 0) {
        return ctx.error;
    }
    if (rc != EMBK_OK) {
        return rc;
    }
    return (int64_t)ctx.count;
}

/* spawn(path) -> handle, or -errno. docs/ARCHITECTURE.md §3.2's spawn()
 * shape: builds a fresh address space directly from an ELF path, no
 * fork+exec. This is a thin wrapper around process_create() (process.c),
 * which already does exactly that -- the only syscall-specific work is
 * copying the path in from user space. The new process is left
 * PROCESS_READY; it isn't run synchronously here, it starts getting real
 * CPU time the next time schedule() (cooperative or timer-driven) picks it
 * up, same as any other READY process. No file-actions list yet (§3.2's
 * fuller model).
 *
 * Returns an opaque per-caller HANDLE (docs/ARCHITECTURE.md §3.4/§3.5), not
 * the raw pid -- process_create() already recorded current_process as the
 * child's parent, so the handle is really just a capability wrapper around
 * that same relationship: only the parent (or whoever it hands the handle
 * to) can ever name this child via sys_wait/sys_kill, closing the
 * confused-deputy gap a bare pid argument would leave open at this
 * boundary (any ring-3 process could otherwise wait()/kill() any pid it
 * could guess). If the handle table is full, the orphaned child is killed
 * immediately rather than leaked unreferenced. */
static int64_t sys_spawn(struct regs *r) {
    const char *user_path = (const char *)r->rdi;
    const uint64_t *user_argv = (const uint64_t *)r->rsi;
    int argc = (int)r->rdx;
    const struct spawn_file_action *user_actions = (const struct spawn_file_action *)r->r10;
    int n_count = (int)r->r8;

    char path[SYSCALL_PATH_MAX];
    int len = copy_string_from_user(path, user_path, sizeof(path));
    if (len < 0) {
        return len;   // -EMBK_EFAULT or -EMBK_ENAMETOOLONG
    }

    /* SPAWN_ARGV_MAX (spawn.h) is the budget for the argv[] pointer array
     * INCLUDING the NULL terminator process_create() appends itself -- so
     * the real string count `argc` must leave room for it, i.e. reject
     * argc == SPAWN_ARGV_MAX, not just argc > SPAWN_ARGV_MAX. */
    if (argc < 0 || argc >= SPAWN_ARGV_MAX || n_count < 0 || n_count > SPAWN_ACTIONS_MAX) {
        return -EMBK_E2BIG;   // caller's request is too large to handle
    }

    /* Copy the argv POINTER ARRAY in first -- these are still PARENT-space
     * pointer (current_process is the parent here; the child will get its own
     * copy in its own address space) */
    uint64_t user_argv_ptrs[SPAWN_ARGV_MAX];
    if (argc > 0 && copy_from_user(user_argv_ptrs, user_argv, argc * sizeof(uint64_t)) != EMBK_OK) {
        return -EMBK_EFAULT;
    }
    
    /* Copy each string into ONE packed kernel buffer -- address-space agnostic
     * once it's here, which is what lets process_create() set up the child's
     * argv correctly without the child's CR3 ever being loaded. 
     * Deliberately NOT static: this syscall can run concurrently on a different
     * core for a different spawn() invocation. static buffer here would let 
     * two spawn() invocations corrupt each other's buffers. */
    char argv_buf[SPAWN_ARGV_BYTES_MAX];
    char *argv_kernel[SPAWN_ARGV_MAX];
    size_t used = 0;
    for (int i = 0; i < argc; i++) {
        int slen = copy_string_from_user(argv_buf + used, (const char *)user_argv_ptrs[i], sizeof(argv_buf) - used);
        if (slen < 0) {
            return slen;   // -EMBK_EFAULT or -EMBK_ENAMETOOLONG
        }
        argv_kernel[i] = argv_buf + used;
        used += (size_t)slen + 1;   // includes the null terminator
        
    }

    struct spawn_file_action actions_kernel[SPAWN_ACTIONS_MAX];
    if (n_count > 0 && copy_from_user(actions_kernel, user_actions,
         n_count * sizeof(struct spawn_file_action)) != EMBK_OK) {
        return -EMBK_EFAULT;
    }

    int pid = process_create(path, argv_kernel, argc, actions_kernel, n_count);
    if (pid < 0) {
        return pid;   // -errno from process_create()
    }
    
    int handle = process_handle_alloc(current_process, (uint32_t)pid);
    if (handle < 0) {
        /* Table full -- almost always because the caller spawned children and
         * never wait()'d the dead ones, leaking a handle slot each. Reclaim any
         * handles that name already-exited children (and reap those zombies),
         * then retry, so a leak-filled table doesn't force us to KILL the valid
         * child we just created. */
        if (process_handle_reap_dead(current_process) > 0) {
            handle = process_handle_alloc(current_process, (uint32_t)pid);
        }
    }
    if (handle < 0) {
        /* Genuinely at capacity -- every handle names a still-LIVE child, so
         * there's no slot to name this one and (with no init-reaper for
         * orphans) leaving it unnamed would leak it. Kill it and report EMFILE. */
        process_kill((uint32_t)pid);
        return handle;   // -EMBK_EMFILE
    }
    return handle;
}

/* wait(handle) -> exit_code, or -errno (-EMBK_EINVAL for a bad/unknown
 * handle, -EMBK_ECHILD if it doesn't/no-longer names one of our children).
 * Resolves the handle to a pid, then blocks on process_wait() (process.c)
 * -- a REAL block via the target's parent's child_wait queue, woken by the
 * child's own exit/kill, not a busy-poll. Frees the handle on return either
 * way (successful reap or a stale/invalid handle): a handle is single-use
 * for waiting, matching the one-shot nature of an exit status. */
static int64_t sys_wait(struct regs *r) {
    int handle = (int)r->rdi;

    uint32_t pid;
    int rc = process_handle_resolve(current_process, handle, &pid);
    if (rc != 0) {
        return rc;   // -EMBK_EINVAL
    }

    int code = process_wait(pid);
    process_handle_free(current_process, handle);
    return code;
}

/* kill(handle) -> 0, or -EMBK_EINVAL for a bad/unknown handle. The
 * userspace-reachable edge of the uncatchable kill
 * (docs/ARCHITECTURE.md §3.3, docs/architecture/process-and-scheduling.md
 * §15.2) -- process_kill() itself (process.c) has existed since Phase B,
 * used internally by the scheduler selftests, but was never exposed to
 * ring 3 until now. Does NOT free the handle: the caller still needs it to
 * sys_wait() afterward and collect the exit code (-1, "killed") the same
 * way a normal exit would be collected. */
static int64_t sys_kill(struct regs *r) {
    int handle = (int)r->rdi;

    uint32_t pid;
    int rc = process_handle_resolve(current_process, handle, &pid);
    if (rc != 0) {
        return rc;   // -EMBK_EINVAL
    }

    process_kill(pid);
    return 0;
}

/* getpid() -> this process's pid. Trivial, but a real primitive a spawn()
 * caller needs -- e.g. to tell itself apart from a child that's about to
 * run the exact same binary from the same entry point. Deliberately still
 * the real pid, not a handle: a process always has ambient authority over
 * itself, so there's no confused-deputy concern for getpid() the way there
 * is for naming some OTHER process via spawn/wait/kill. */
static int64_t sys_getpid(struct regs *r) {
    (void)r;
    return current_process->pid;
}


/* exit(code): mark the process a zombie and hand off to the scheduler.
 * process_exit_self() (process.c) does the actual work and is shared with
 * the in-kernel scheduler selftests, which need the identical mark-zombie-
 * then-reschedule path without going through a syscall. */
static int64_t sys_exit(struct regs *r) {
    /* The exit code is the syscall's first ARGUMENT, in rdi (same slot
     * sys_write's fd comes from) -- NOT rax, which still holds the syscall
     * NUMBER (2, for every SYS_exit call) at this point. Reading rax here
     * used to silently discard every real exit code and replace it with 2. */
    int code = (int)r->rdi;
    serial_write_string("\n[syscall] exit code=");
    serial_write_hex(code);
    serial_write_string("\n");

    process_exit_self(code);   // noreturn
}

/* yield(): voluntarily give up the CPU to the next READY process. process.c's
 * sys_yield() is void (no args, no return value) since it's also called
 * internally; wrap it to match the syscall_handler_t signature. */
static int64_t sys_yield_syscall(struct regs *r) {
    (void)r;
    sys_yield();
    return 0;
}

/* thread_create(entry, arg) -> tid, or -errno. The ring-3 edge of Phase 5's
 * multi-thread primitive (docs/architecture/process-and-scheduling.md) --
 * an ADDITIONAL thread under the CALLER's own process, sharing its address
 * space, with its own dedicated user stack (thread_create_user(), process.c).
 *
 * `entry` is validated with access_ok() before ever being handed to the
 * scheduler: unlike sys_spawn's path (a fresh ELF's own entry point, chosen
 * by the trusted loader, not by the caller), this entry point is an
 * ARBITRARY ring-3-supplied address, so it gets the same "must be mapped in
 * the CALLER's own address space" check every other user pointer this
 * syscall layer accepts gets (usercopy.h) -- cheap insurance against a
 * caller (accidentally or not) pointing a brand-new thread at unmapped or
 * kernel-half memory before it ever gets to run.
 *
 * Returns a raw tid (thread_table[] slot index), not a capability handle --
 * see thread_create_user()'s doc comment (process.h) for why a thread
 * doesn't need the same confused-deputy protection sys_spawn's handle does:
 * a tid only ever names a thread of the CALLER's OWN process, there's no
 * cross-process thread naming at all. */
static int64_t sys_thread_create(struct regs *r) {
    uint64_t entry_point = r->rdi;
    uint64_t arg = r->rsi;

    if (!access_ok((const void *)entry_point, 1)) {
        return -EMBK_EFAULT;
    }

    return thread_create_user(current_process, entry_point, arg);
}

/* thread_join(tid) -> exit_code, or -errno. Blocks (a real block, via
 * proc->join_wait -- not a busy-poll, same shape as sys_wait/process_wait())
 * until thread `tid` of the CALLER's own process exits, then reaps it and
 * returns its exit code. -EMBK_EINVAL for an unknown/wrong-process/
 * non-joinable/already-joined tid (thread_join(), process.c). */
static int64_t sys_thread_join(struct regs *r) {
    int tid = (int)r->rdi;
    return thread_join(current_process, tid);
}

/* thread_exit(code): mark the CALLING thread a zombie and hand off to the
 * scheduler -- ends only this thread, not necessarily the whole process
 * (thread_exit_self(), process.c). If it happens to be the process's last
 * thread, this completes the process exactly like sys_exit would. */
static int64_t sys_thread_exit(struct regs *r) {
    int code = (int)r->rdi;
    thread_exit_self(code);   // noreturn
}

/* sbrk(increment) -> new break pointer, or -errno. Adjusts the current
 * process's heap break pointer (current_process->heap_brk) by the signed
 * `increment` amount, growing or shrinking the heap. Returns the new break
 * pointer on success, or -errno on failure (e.g., exceeding USER_HEAP_VA_MAX
 * or underflowing below USER_HEAP_VA_BASE). */
static int64_t sys_sbrk(struct regs *r) {
    int64_t increment = (int64_t)r->rdi;
    return process_sbrk(current_process, increment);
}

/* fstat(fd, out_stat) -> 0, or -errno. The fd-based companion to sys_stat's
 * path-based lookup -- newlib's stdio layer calls fstat() on every open
 * stream (to size its buffer / decide line- vs block-buffering), so a libc
 * port can't get far without it. Fills the kernel's neutral struct vfs_stat
 * (vfs.h); the userland _fstat stub maps that to newlib's struct stat.
 *
 * fds 0/1/2 aren't real vnodes (no console vnode yet -- see sys_write), so
 * vfs_fd_fstat() rejects them; the userland stub synthesizes a character-
 * device stat for those itself rather than calling here (the standard
 * newlib "_fstat on a tty returns S_IFCHR" shortcut), so this only ever
 * sees real fd >= FD_BASE. */
static int64_t sys_fstat(struct regs *r) {
    int fd = (int)r->rdi;
    struct vfs_stat *user_out = (struct vfs_stat *)r->rsi;

    struct vfs_stat st;
    int rc = vfs_fd_fstat(fd, &st);
    if (rc != EMBK_OK) {
        return rc;
    }
    if (copy_to_user(user_out, &st, sizeof(st)) != EMBK_OK) {
        return -EMBK_EFAULT;
    }
    return 0;
}

/* gettimeofday(out) -> 0, or -errno. Writes two uint64s -- {seconds,
 * microseconds} since the Unix epoch -- into the caller's buffer, sourced
 * from the CMOS RTC (rtc_now_unix(), the only wall-clock this kernel has).
 * microseconds is ALWAYS 0: a CMOS RTC's real resolution is one second
 * (rtc.h documents this precision gap), so fabricating sub-second digits
 * here would be dishonest. The userland _gettimeofday stub maps this pair
 * onto newlib's struct timeval; time() is then just the seconds field. */
static int64_t sys_gettimeofday(struct regs *r) {
    uint64_t *user_out = (uint64_t *)r->rdi;   // [0]=sec, [1]=usec

    uint64_t tv[2];
    tv[0] = rtc_now_unix();
    tv[1] = 0;
    if (copy_to_user(user_out, tv, sizeof(tv)) != EMBK_OK) {
        return -EMBK_EFAULT;
    }
    return 0;
}

/* ---- Surfaces (EmbLink UI Piece 1: shared-memory pixel buffers) ---------
 * Thin wrappers over kernel/gfx/surface.c. current_process is the client for
 * create; the caller for the rest. See surface.h and the design spec. */

static int64_t sys_surface_create(struct regs *r) {
    uint32_t w = (uint32_t)r->rdi, h = (uint32_t)r->rsi;
    uint32_t fmt = (uint32_t)r->rdx, n = (uint32_t)r->r10;
    struct surface_info *user_out = (struct surface_info *)r->r8;

    struct surface_info info;
    int handle = surface_create(current_process, w, h, fmt, n, &info);
    if (handle < 0) {
        return handle;
    }
    if (user_out && copy_to_user(user_out, &info, sizeof(info)) != EMBK_OK) {
        surface_destroy(current_process, handle);   // don't leak a half-handed-out surface
        return -EMBK_EFAULT;
    }
    return handle;
}

static int64_t sys_surface_map(struct regs *r) {
    int handle = (int)r->rdi;
    struct surface_info *user_out = (struct surface_info *)r->rsi;

    struct surface_info info;
    uint64_t base = surface_map(current_process, handle, &info);
    if (!base) {
        return -EMBK_EINVAL;
    }
    if (user_out && copy_to_user(user_out, &info, sizeof(info)) != EMBK_OK) {
        return -EMBK_EFAULT;
    }
    return (int64_t)base;
}

static int64_t sys_surface_acquire(struct regs *r) {
    return surface_acquire(current_process, (int)r->rdi);
}
static int64_t sys_surface_commit(struct regs *r) {
    return surface_commit(current_process, (int)r->rdi, (int)r->rsi);
}
static int64_t sys_surface_release(struct regs *r) {
    return surface_release(current_process, (int)r->rdi, (int)r->rsi);
}
static int64_t sys_surface_destroy(struct regs *r) {
    return surface_destroy(current_process, (int)r->rdi);
}

/* ---- Direct present: blit a user BGRA8888 buffer to the framebuffer -------
 * A minimal "get pixels on screen" path for a single full-screen UI process,
 * standing in for a full compositor. Copies row-by-row via copy_from_user
 * (safe against a bad/short user pointer) and centres the image, then presents.
 * rdi=user pixels, rsi=w, rdx=h. Source pixels are premultiplied BGRA (== a
 * little-endian 0xAARRGGBB uint32); fb_blit's opaque path copies the RGB. */
static int64_t sys_ui_present(struct regs *r) {
    const uint8_t *upx = (const uint8_t *)r->rdi;
    uint32_t w = (uint32_t)r->rsi, h = (uint32_t)r->rdx;
    const fb_info_t *fi = fb_get_info();
    if (!fi || w == 0 || h == 0 || w > fi->width || h > fi->height) return -EMBK_EINVAL;

    uint32_t *line = (uint32_t *)kmalloc((size_t)w * 4);
    if (!line) return -EMBK_ENOMEM;
    int32_t x = (int32_t)(fi->width  - w) / 2;
    int32_t y = (int32_t)(fi->height - h) / 2;
    for (uint32_t row = 0; row < h; row++) {
        if (copy_from_user(line, upx + (size_t)row * w * 4, (size_t)w * 4) != EMBK_OK) {
            kfree(line);
            return -EMBK_EFAULT;
        }
        fb_blit(x, y + (int32_t)row, (int32_t)w, 1, line, w);
    }
    kfree(line);
    fb_present();
    return 0;
}

/* Present only a sub-rectangle of the user surface to the framebuffer -- the
 * interactive path: an app that only moved its cursor uploads a handful of
 * rows instead of the whole surface (the full blit above dominates a TCG
 * frame). Args are packed to fit the register ABI:
 *   rdi = pixel base, rsi = (surf_w<<16)|surf_h,
 *   rdx = (rx<<48)|(ry<<32)|(rw<<16)|rh  (each 16-bit, surface-local). */
static int64_t sys_ui_present_rect(struct regs *r) {
    const uint8_t *upx = (const uint8_t *)r->rdi;
    uint32_t sw = ((uint32_t)r->rsi >> 16) & 0xFFFF, sh = (uint32_t)r->rsi & 0xFFFF;
    uint64_t rp = (uint64_t)r->rdx;
    uint32_t rx = (uint32_t)((rp >> 48) & 0xFFFF), ry = (uint32_t)((rp >> 32) & 0xFFFF);
    uint32_t rw = (uint32_t)((rp >> 16) & 0xFFFF), rh = (uint32_t)(rp & 0xFFFF);
    const fb_info_t *fi = fb_get_info();
    if (!fi || sw == 0 || sh == 0 || sw > fi->width || sh > fi->height) return -EMBK_EINVAL;
    if (rx >= sw || ry >= sh || rw == 0 || rh == 0) return 0;   /* empty -> nothing to push */
    if (rx + rw > sw) rw = sw - rx;
    if (ry + rh > sh) rh = sh - ry;

    uint32_t *line = (uint32_t *)kmalloc((size_t)rw * 4);
    if (!line) return -EMBK_ENOMEM;
    int32_t x0 = (int32_t)(fi->width  - sw) / 2;
    int32_t y0 = (int32_t)(fi->height - sh) / 2;
    for (uint32_t row = 0; row < rh; row++) {
        const uint8_t *srow = upx + ((size_t)(ry + row) * sw + rx) * 4;
        if (copy_from_user(line, srow, (size_t)rw * 4) != EMBK_OK) {
            kfree(line);
            return -EMBK_EFAULT;
        }
        fb_blit(x0 + (int32_t)rx, y0 + (int32_t)(ry + row), (int32_t)rw, 1, line, rw);
    }
    kfree(line);
    fb_present();
    return 0;
}

/* Poll current pointer state into a user struct {int32 x,y; uint32 buttons}.
 * The ring-3 UI loop calls this each frame to drive hover/click. */
struct ui_input_kbuf { int32_t x, y; uint32_t buttons; int32_t wheel; };
static int64_t sys_ui_input(struct regs *r) {
    struct ui_input_kbuf k;
    int32_t x = 0, y = 0; uint32_t b = 0;
    mouse_get_state(&x, &y, &b);
    k.x = x; k.y = y; k.buttons = b; k.wheel = mouse_take_wheel();
    if (copy_to_user((void *)r->rdi, &k, sizeof k) != EMBK_OK) return -EMBK_EFAULT;
    return 0;
}

/* Non-blocking keystroke poll: returns the next ASCII byte (incl. '\b' 0x08 and
 * '\n'), or 0 if the keyboard buffer is empty. The ring-3 UI loop drains this
 * each frame and routes chars to the focused text field. */
static int64_t sys_key_poll(struct regs *r) {
    (void)r;
    if (keyboard_has_char()) return (int64_t)(unsigned char)keyboard_getchar();
    return 0;
}

/* Grab/release the keyboard so the kernel shell stops draining it while a UI app
 * owns keystrokes (rdi != 0 grabs). Auto-released when this process is reaped. */
static int64_t sys_key_grab(struct regs *r) {
    keyboard_set_grab(r->rdi != 0, current_process ? current_process->pid : 0);
    return 0;
}

/* Monotonic milliseconds since boot, from the HPET counter -- the clock the
 * ring-3 UI animator ticks on. Falls back to the coarse timer tick count. */
static uint64_t uptime_ms_now(void) {
    if (hpet_available()) {
        uint64_t pf = hpet_period_fs();                 /* femtoseconds per tick */
        if (pf) {
            uint64_t tpms = 1000000000000ULL / pf;      /* ticks per millisecond */
            if (tpms == 0) tpms = 1;
            return hpet_read_counter() / tpms;
        }
    }
    return timer_get_ticks();
}

static int64_t sys_uptime_ms(struct regs *r) {
    (void)r;
    return (int64_t)uptime_ms_now();
}

/* Sleep >= rdi milliseconds. Implemented as a yield loop against an HPET
 * deadline -- every pass gives the CPU away, so a sleeping app costs a few
 * microseconds per scheduler round instead of burning its whole timeslice
 * (the ring-3 UI apps used to pace with volatile spin loops; with 2-3 apps
 * live, each stole a full slice per round and everything crawled). Not a
 * blocking timer wait (no scheduler-side wakeup list -- deliberately zero
 * scheduler surgery), but it removes ~all of the idle CPU theft. */
static int64_t sys_sleep_ms(struct regs *r) {
    uint64_t end = uptime_ms_now() + r->rdi;
    do { sys_yield(); } while (uptime_ms_now() < end);
    return 0;
}

/* 1 if the child named by spawn HANDLE `rdi` is alive, else 0. Takes the same
 * handle sys_spawn returned (NOT a raw pid -- spawn deliberately never exposes
 * pids), so the check is pinned to the exact child instance: a recycled pid
 * can't alias, and an unknown/freed handle is simply "not alive". The home
 * launcher uses this to not re-spawn an app whose tile is clicked twice. */
static int64_t sys_proc_alive(struct regs *r) {
    uint32_t pid;
    if (process_handle_resolve(current_process, (int)r->rdi, &pid) != 0)
        return 0;
    return (int64_t)process_alive(pid);
}

/* win_resize(id, w, h, &out_va) -> id, with the window's NEW shared-pixel
 * mapping base written to *out_va (the old mapping is gone on return). */
static int64_t sys_win_resize(struct regs *r) {
    uint64_t cva = 0;
    int64_t rc = compositor_win_resize(current_process, (uint32_t)r->rdi,
                                       (uint32_t)r->rsi, (uint32_t)r->rdx, &cva);
    if (rc < 0) return rc;
    if (r->r10 && copy_to_user((void *)r->r10, &cva, sizeof(cva)) != EMBK_OK)
        return -EMBK_EFAULT;
    return rc;
}

/* ---- window compositor (EmbLink UI Piece 2) -----------------------------
 * Thin wrappers over kernel/gfx/compositor.c. The client renders into its own
 * buffer and PRESENTS pixels (copy_from_user) into a kernel window content
 * buffer; the compositor draws all windows over a desktop with chrome. */

/* rdi=content_w, rsi=content_h, rdx=x, r10=y, r8=title(user char*),
 * r9=out uint64_t* for the shared client VA (0 => plain copy window).
 * Returns a window id (>0) or -EMBK_*. */
static int64_t sys_win_create(struct regs *r) {
    /* Window-style flags ride the high bits of rdi (cw is <= 16 bits real). */
    uint32_t cw = (uint32_t)(r->rdi & 0xFFFFFFFFULL), ch = (uint32_t)r->rsi;
    int chromeless = (r->rdi >> 32) & 1;   /* EMBK_WINF_CHROMELESS */
    int widget     = (r->rdi >> 33) & 1;   /* EMBK_WINF_WIDGET */
    int glass      = (r->rdi >> 34) & 1;   /* EMBK_WINF_GLASS */
    int32_t x = (int32_t)r->rdx, y = (int32_t)r->r10;
    char title[COMP_TITLE_MAX + 1];
    int i = 0;
    if (r->r8) {
        for (; i < COMP_TITLE_MAX; i++) {
            char c;
            if (copy_from_user(&c, (const void *)(r->r8 + (uint64_t)i), 1) != EMBK_OK) break;
            if (!c) break;
            title[i] = c;
        }
    }
    title[i] = 0;
    int pid = current_process ? (int)current_process->pid : -1;

    if (r->r9) {   /* zero-copy: map the pixel pages into the client, return VA */
        uint64_t cva = 0;
        int64_t id = widget
            ? compositor_win_create_widget(current_process, cw, ch, x, y, title, glass, &cva)
            : glass
            ? compositor_win_create_glass(current_process, cw, ch, x, y, title, &cva)
            : chromeless
            ? compositor_win_create_chromeless(current_process, cw, ch, x, y, title, &cva)
            : compositor_win_create_shared(current_process, cw, ch, x, y, title, &cva);
        if (id < 0) return id;
        if (copy_to_user((void *)r->r9, &cva, sizeof(cva)) != EMBK_OK) {
            compositor_win_destroy(pid, (uint32_t)id);
            return -EMBK_EFAULT;
        }
        return id;
    }
    return compositor_win_create(pid, cw, ch, x, y, title);
}

/* rdi=win id, rsi=user pixels (whole cw*ch content), rdx=(cw<<16)|ch,
 * r10=(rx<<48)|(ry<<32)|(rw<<16)|rh  (0 => present whole window). */
static int64_t sys_win_present(struct regs *r) {
    uint32_t id = (uint32_t)r->rdi;
    const uint8_t *upx = (const uint8_t *)r->rsi;
    uint32_t uw = ((uint32_t)r->rdx >> 16) & 0xFFFF, uh = (uint32_t)r->rdx & 0xFFFF;
    int pid = current_process ? (int)current_process->pid : -1;

    uint32_t cw = 0, ch = 0;
    uint32_t *content = compositor_win_content(pid, id, &cw, &ch);
    if (!content) return -EMBK_ENOENT;
    if (uw != cw || uh != ch) return -EMBK_EINVAL;   /* client/kernel disagree */

    uint64_t rp = (uint64_t)r->r10;
    uint32_t rx, ry, rw, rh;
    if (rp == 0) { rx = 0; ry = 0; rw = cw; rh = ch; }
    else {
        rx = (uint32_t)((rp >> 48) & 0xFFFF); ry = (uint32_t)((rp >> 32) & 0xFFFF);
        rw = (uint32_t)((rp >> 16) & 0xFFFF); rh = (uint32_t)(rp & 0xFFFF);
    }
    if (rx >= cw || ry >= ch || rw == 0 || rh == 0) return 0;
    if (rx + rw > cw) rw = cw - rx;
    if (ry + rh > ch) rh = ch - ry;

    /* A shared (zero-copy) window's client already rendered straight into the
     * shared pages -- there's nothing to copy, just damage. A plain window
     * uploads the presented rows from the client's private buffer. */
    if (!compositor_win_is_shared(pid, id)) {
        for (uint32_t row = 0; row < rh; row++) {
            size_t off = ((size_t)(ry + row) * cw + rx);
            if (copy_from_user(content + off, upx + off * 4, (size_t)rw * 4) != EMBK_OK)
                return -EMBK_EFAULT;
        }
    }
    return compositor_win_damage(pid, id, rx, ry, rw, rh);
}

/* rdi=win id, rsi=x, rdx=y (screen coords of the window frame's top-left). */
static int64_t sys_win_move(struct regs *r) {
    int pid = current_process ? (int)current_process->pid : -1;
    return compositor_win_move(pid, (uint32_t)r->rdi, (int32_t)r->rsi, (int32_t)r->rdx);
}

/* rdi=win id. */
static int64_t sys_win_destroy(struct regs *r) {
    int pid = current_process ? (int)current_process->pid : -1;
    return compositor_win_destroy(pid, (uint32_t)r->rdi);
}

/* Report the screen (framebuffer) size so a windowed app can fit itself to it.
 * Writes width to *[rdi] and height to *[rsi]. */
static int64_t sys_screen_size(struct regs *r) {
    const fb_info_t *fi = fb_get_info();
    uint32_t w = fi ? fi->width : 0, h = fi ? fi->height : 0;
    if ((r->rdi && copy_to_user((void *)r->rdi, &w, sizeof w) != EMBK_OK) ||
        (r->rsi && copy_to_user((void *)r->rsi, &h, sizeof h) != EMBK_OK))
        return -EMBK_EFAULT;
    return 0;
}

/* Create the full-screen chromeless HOME/desktop window (zero-copy). Returns the
 * window id; writes the client pixel VA to *[rdi] and the screen size to *[rsi]
 * (w) and *[rdx] (h) so the app learns the dimensions. */
static int64_t sys_win_create_desktop(struct regs *r) {
    uint64_t cva = 0; uint32_t w = 0, h = 0;
    int64_t id = compositor_win_create_desktop(current_process, &cva, &w, &h);
    if (id < 0) return id;
    int pid = current_process ? (int)current_process->pid : -1;
    if ((r->rdi && copy_to_user((void *)r->rdi, &cva, sizeof cva) != EMBK_OK) ||
        (r->rsi && copy_to_user((void *)r->rsi, &w, sizeof w) != EMBK_OK) ||
        (r->rdx && copy_to_user((void *)r->rdx, &h, sizeof h) != EMBK_OK)) {
        compositor_win_destroy(pid, (uint32_t)id);
        return -EMBK_EFAULT;
    }
    return id;
}

/* rdi = user ptr to struct { int32_t focused; int32_t x, y; uint32_t buttons,
 * win; }. Delivers the content-local pointer if this process owns the window
 * under the cursor (focused=1) else focused=0. */
struct win_input_kbuf { int32_t focused; int32_t x, y; uint32_t buttons; uint32_t win; int32_t wheel; };
static int64_t sys_win_input(struct regs *r) {
    int pid = current_process ? (int)current_process->pid : -1;
    int32_t lx = 0, ly = 0, wheel = 0; uint32_t btn = 0, win = 0;
    int foc = compositor_win_input(pid, &lx, &ly, &btn, &win, &wheel);
    struct win_input_kbuf k = { foc, lx, ly, btn, win, wheel };
    if (copy_to_user((void *)r->rdi, &k, sizeof k) != EMBK_OK) return -EMBK_EFAULT;
    return 0;
}

/* ---- IPC channels (EmbLink UI Piece 1, Layer A) -------------------------
 * The syscall layer does the user<->kernel copies; kernel/ipc/channel.c does
 * the queueing, blocking, and handle transfer. */

static int64_t sys_chan_pair(struct regs *r) {
    int *user_out = (int *)r->rdi;   /* -> int[2] */
    int h0 = -1, h1 = -1;
    int rc = channel_create_pair(current_process, &h0, &h1);
    if (rc < 0) return rc;
    int out[2] = { h0, h1 };
    if (copy_to_user(user_out, out, sizeof(out)) != EMBK_OK) {
        channel_close(current_process, h0);
        channel_close(current_process, h1);
        return -EMBK_EFAULT;
    }
    return 0;
}

static int64_t sys_chan_send(struct regs *r) {
    int handle       = (int)r->rdi;
    const void *ubytes = (const void *)r->rsi;
    uint32_t len     = (uint32_t)r->rdx;
    const int *uhnds = (const int *)r->r10;
    const int *uflags= (const int *)r->r8;
    uint32_t n_hnd   = (uint32_t)r->r9;

    if (len > CHAN_MSG_MAX_BYTES || n_hnd > CHAN_MSG_MAX_HANDLES) return -EMBK_EINVAL;

    uint8_t kbytes[CHAN_MSG_MAX_BYTES];
    if (len && copy_from_user(kbytes, ubytes, len) != EMBK_OK) return -EMBK_EFAULT;

    int khnds[CHAN_MSG_MAX_HANDLES];
    int kflags[CHAN_MSG_MAX_HANDLES];
    if (n_hnd) {
        if (copy_from_user(khnds, uhnds, n_hnd * sizeof(int)) != EMBK_OK) return -EMBK_EFAULT;
        if (uflags) {
            if (copy_from_user(kflags, uflags, n_hnd * sizeof(int)) != EMBK_OK) return -EMBK_EFAULT;
        } else {
            for (uint32_t i = 0; i < n_hnd; i++) kflags[i] = CHAN_HANDLE_COPY;
        }
    }
    return channel_send(current_process, handle, len ? kbytes : 0, len,
                        n_hnd ? khnds : 0, n_hnd ? kflags : 0, n_hnd);
}

static int64_t sys_chan_recv(struct regs *r) {
    int handle          = (int)r->rdi;
    void *ubuf          = (void *)r->rsi;
    uint32_t buflen     = (uint32_t)r->rdx;
    uint32_t *u_outlen  = (uint32_t *)r->r10;
    int *u_outhnds      = (int *)r->r8;
    uint32_t *u_outnhnd = (uint32_t *)r->r9;

    /* Effective limit = min(user buflen, kernel buffer). channel_recv returns
     * EMSGSIZE (message left queued) if the message exceeds it, so we never
     * overrun the user buffer. */
    uint32_t eff = buflen > CHAN_MSG_MAX_BYTES ? CHAN_MSG_MAX_BYTES : buflen;
    uint8_t kbuf[CHAN_MSG_MAX_BYTES];
    uint32_t out_len = 0, out_nhnd = 0;
    int out_hnds[CHAN_MSG_MAX_HANDLES];

    int64_t rc = channel_recv(current_process, handle, kbuf, eff, &out_len, out_hnds, &out_nhnd);
    if (rc < 0) return rc;

    if (out_len && ubuf && copy_to_user(ubuf, kbuf, out_len) != EMBK_OK) return -EMBK_EFAULT;
    if (u_outlen && copy_to_user(u_outlen, &out_len, sizeof(out_len)) != EMBK_OK) return -EMBK_EFAULT;
    if (out_nhnd && u_outhnds &&
        copy_to_user(u_outhnds, out_hnds, out_nhnd * sizeof(int)) != EMBK_OK) return -EMBK_EFAULT;
    if (u_outnhnd && copy_to_user(u_outnhnd, &out_nhnd, sizeof(out_nhnd)) != EMBK_OK) return -EMBK_EFAULT;
    return 0;
}

static int64_t sys_chan_close(struct regs *r) {
    return channel_close(current_process, (int)r->rdi);
}

/* ---- Rendezvous (EmbLink UI Piece 1, Layer B) ---------------------------
 * VFS named listening endpoints (epfs, mounted at /run). `path` copied in
 * through a bounded kernel buffer, same pattern as sys_open/sys_stat. */

static int64_t sys_chan_listen(struct regs *r) {
    const char *user_path = (const char *)r->rdi;
    char path[SYSCALL_PATH_MAX];
    int len = copy_string_from_user(path, user_path, sizeof(path));
    if (len < 0) return len;
    return endpoint_listen(current_process, path);
}

static int64_t sys_chan_accept(struct regs *r) {
    return endpoint_accept(current_process, (int)r->rdi);
}

static int64_t sys_chan_connect(struct regs *r) {
    const char *user_path = (const char *)r->rdi;
    char path[SYSCALL_PATH_MAX];
    int len = copy_string_from_user(path, user_path, sizeof(path));
    if (len < 0) return len;
    return endpoint_connect(current_process, path);
}

/* --- The table: index = syscall number --- */
typedef int64_t (*syscall_handler_t)(struct regs *);

#define SYS_write   1
#define SYS_exit    2
#define SYS_yield   3
#define SYS_open    4
#define SYS_close   5
#define SYS_read    6
#define SYS_lseek   7
#define SYS_stat    8
#define SYS_readdir 9
#define SYS_spawn   10
#define SYS_wait    11
#define SYS_getpid  12
#define SYS_kill    13
#define SYS_thread_create 14
#define SYS_thread_join   15
#define SYS_thread_exit   16
#define SYS_sbrk    17
#define SYS_fstat   18
#define SYS_gettimeofday 19
#define SYS_surface_create  20
#define SYS_surface_map     21
#define SYS_surface_acquire 22
#define SYS_surface_commit  23
#define SYS_surface_release 24
#define SYS_surface_destroy 25
#define SYS_chan_pair   26
#define SYS_chan_send   27
#define SYS_chan_recv   28
#define SYS_chan_close  29
#define SYS_chan_listen  30
#define SYS_chan_accept  31
#define SYS_chan_connect 32
#define SYS_ui_present   33
#define SYS_ui_input     34
#define SYS_ui_present_rect 35
#define SYS_key_poll     36
#define SYS_key_grab     37
#define SYS_uptime_ms    38
#define SYS_win_create   39
#define SYS_win_present  40
#define SYS_win_move     41
#define SYS_win_destroy  42
#define SYS_win_create_desktop 43
#define SYS_win_input    44
#define SYS_screen_size  45
#define SYS_sleep_ms     46
#define SYS_proc_alive   47
#define SYS_win_resize   48


static syscall_handler_t syscall_table[] = {
    [SYS_write]   = sys_write,
    [SYS_exit]    = sys_exit,
    [SYS_yield]   = sys_yield_syscall,
    [SYS_open]    = sys_open,
    [SYS_close]   = sys_close,
    [SYS_read]    = sys_read,
    [SYS_lseek]   = sys_lseek,
    [SYS_stat]    = sys_stat,
    [SYS_readdir] = sys_readdir,
    [SYS_spawn]   = sys_spawn,
    [SYS_wait]    = sys_wait,
    [SYS_getpid]  = sys_getpid,
    [SYS_kill]    = sys_kill,
    [SYS_thread_create] = sys_thread_create,
    [SYS_thread_join]   = sys_thread_join,
    [SYS_thread_exit]   = sys_thread_exit,
    [SYS_sbrk]    = sys_sbrk,
    [SYS_fstat]   = sys_fstat,
    [SYS_gettimeofday] = sys_gettimeofday,
    [SYS_surface_create]  = sys_surface_create,
    [SYS_surface_map]     = sys_surface_map,
    [SYS_surface_acquire] = sys_surface_acquire,
    [SYS_surface_commit]  = sys_surface_commit,
    [SYS_surface_release] = sys_surface_release,
    [SYS_surface_destroy] = sys_surface_destroy,
    [SYS_chan_pair]  = sys_chan_pair,
    [SYS_chan_send]  = sys_chan_send,
    [SYS_chan_recv]  = sys_chan_recv,
    [SYS_chan_close] = sys_chan_close,
    [SYS_chan_listen]  = sys_chan_listen,
    [SYS_chan_accept]  = sys_chan_accept,
    [SYS_chan_connect] = sys_chan_connect,
    [SYS_ui_present]   = sys_ui_present,
    [SYS_ui_input]     = sys_ui_input,
    [SYS_ui_present_rect] = sys_ui_present_rect,
    [SYS_key_poll]     = sys_key_poll,
    [SYS_uptime_ms]    = sys_uptime_ms,
    [SYS_key_grab]     = sys_key_grab,
    [SYS_win_create]   = sys_win_create,
    [SYS_win_present]  = sys_win_present,
    [SYS_win_move]     = sys_win_move,
    [SYS_win_destroy]  = sys_win_destroy,
    [SYS_win_create_desktop] = sys_win_create_desktop,
    [SYS_win_input]    = sys_win_input,
    [SYS_screen_size]  = sys_screen_size,
    [SYS_sleep_ms]     = sys_sleep_ms,
    [SYS_proc_alive]   = sys_proc_alive,
    [SYS_win_resize]   = sys_win_resize,
};

#define SYSCALL_TABLE_SIZE (sizeof(syscall_table) / sizeof(syscall_handler_t))

/* Called from the asm stub (syscall_entry.asm) with a pointer to the saved
 * register frame. Must have external linkage so the assembler's
 * `extern syscall_dispatch` resolves — a `static` here would not link.
 * Number in rax; result written back into rax (the stub pops it to the user).*/
void syscall_dispatch(struct regs *r) {
    /* int 0x80 is an INTERRUPT gate (IDT_GATE_USER = 0xEE, type 0xE), which
     * auto-clears IF on entry -- so without this, every syscall handler
     * below runs with interrupts disabled for its entire duration. That's
     * fine for sys_write/sys_exit (no blocking work), but sys_open and
     * friends go through vfs_open() -> EMBKFS/FAT32 -> the block layer ->
     * ATA/AHCI, which waits on a disk-completion IRQ to finish a read --
     * an IRQ that can never be serviced while IF=0. Without this `sti`,
     * any syscall that touches disk I/O hangs forever.
     * Safe to re-enable here: the asm stub's `iret` at the end restores
     * RIP/CS/RFLAGS from the ORIGINAL int 0x80 entry frame regardless of
     * what IF is doing in between, and ring-3 code always runs with IF=1
     * anyway (it can't execute cli/sti itself -- privileged, #GP). */
    __asm__ volatile("sti");

    uint64_t syscall_number = r->rax;
    if (syscall_number < SYSCALL_TABLE_SIZE && syscall_table[syscall_number]) {
        r->rax = (uint64_t)syscall_table[syscall_number](r);
    } else {
        r->rax = (uint64_t)(-EMBK_EINVAL); // Invalid syscall number
    }
}


/* type_attr byte for a 64-bit IDT gate: P(0x80) | DPL(bits 5-6) | type(0xE =
 * interrupt gate, which auto-clears IF on entry). The CS selector (0x08) is set
 * by idt_set_entry itself. */
#define IDT_GATE_KERNEL  0x8E   /* present, DPL0, interrupt gate */
#define IDT_GATE_USER    0xEE   /* present, DPL3, interrupt gate (0x8E | 0x60) */

#define IST_DOUBLE_FAULT 1      /* TSS IST slot for #DF; g_tss.ist1 set in gdt_init */

void syscall_init(void) {
    extern void syscall_entry(void); // Defined in syscall_entry.asm
    /* Vector 0x80, interrupt gate (auto-clears IF). DPL=3 so a ring-3
     * `int 0x80` is allowed; IST 0 means use RSP0 from the TSS. */
    idt_set_entry(0x80, (uint64_t)syscall_entry, IDT_GATE_USER);

    /* Upgrade #DF (vector 8) to run on IST1. idt_init installs a baseline isr8
     * on the regular stack; here we re-point vector 8 at the dedicated
     * isr_double_fault entry that switches to g_tss.ist1 (set up in gdt_init).
     * That way a malformed frame yields a handler+dump instead of reusing a bad
     * kernel stack and triple-faulting into a reset. Must run after idt_init. */
    extern void isr_double_fault(void);
    idt_set_entry_ist(8, (uint64_t)isr_double_fault, IDT_GATE_KERNEL, IST_DOUBLE_FAULT);
}
