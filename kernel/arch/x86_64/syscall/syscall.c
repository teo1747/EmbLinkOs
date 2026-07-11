#include "arch/x86_64/syscall/syscall.h"
#include "arch/x86_64/irq/idt.h"
#include "arch/x86_64/cpu/kcontext.h"
#include "arch/x86_64/syscall/usercopy.h"
#include "drivers/char/serial.h"
#include "drivers/timer/rtc.h"
#include "include/errno.h"
#include "include/types.h"
#include "include/kstring.h"
#include "process/process.h"
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
        if (fd == 1 || fd == 2) {
            /* stdout (1) AND stderr (2) both go to the serial console -- newlib
             * writes diagnostics to fd 2, and without routing it here every
             * stderr write would fall through to vfs_fd_write(2, ...) and fail
             * (fd 2 < FD_BASE). No separate error stream to split them onto
             * yet; both land on the same console. */
            for (size_t i = 0; i < n; i++) {
                serial_write_char(chunk[i]);
            }
            advanced = n;   // serial writes always take the whole chunk
        } else {
            size_t written = 0;
            int rc = vfs_fd_write(fd, chunk, n, &written);
            if (rc != EMBK_OK) {
                return done > 0 ? (int64_t)done : rc;
            }
            advanced = written;
        }

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

    if (fd == 0 || fd == 1 || fd == 2) {
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
        process_kill((uint32_t)pid);   // orphaned child, no handle to name it
        return handle;   // -errno from process_handle_alloc()
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
