#include "syscall.h"
#include "idt.h"
#include "kcontext.h"
#include "usercopy.h"
#include "../drivers/serial.h"
#include "../include/errno.h"
#include "../include/types.h"
#include "../include/kstring.h"
#include "../process/process.h"
#include "../fs/fd.h"
#include "../fs/vfs.h"

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
        if (fd == 1) {
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

/* spawn(path) -> pid, or -errno. docs/ARCHITECTURE.md §3.2's spawn() shape:
 * builds a fresh address space directly from an ELF path, no fork+exec. This
 * is a thin wrapper around process_create() (process.c), which already does
 * exactly that -- the only syscall-specific work is copying the path in
 * from user space. The new process is left PROCESS_READY; it isn't run
 * synchronously here, it starts getting real CPU time the next time
 * schedule() (cooperative or timer-driven) picks it up, same as any other
 * READY process. No file-actions list yet (§3.2's fuller model) and no
 * capability-style handle (§3.4) -- returns the raw pid, the simplest thing
 * that works given neither exists yet. */
static int64_t sys_spawn(struct regs *r) {
    const char *user_path = (const char *)r->rdi;

    char path[SYSCALL_PATH_MAX];
    int len = copy_string_from_user(path, user_path, sizeof(path));
    if (len < 0) {
        return len;
    }

    return process_create(path);
}

/* wait(pid) -> exit_code, or -errno (-EMBK_ECHILD if no such pid). Busy-
 * polls (yielding every round) until the target reaches PROCESS_ZOMBIE,
 * then reaps it and returns its exit code.
 *
 * This is a real, working wait(), but not the eventual one: a proper
 * implementation blocks the caller on a per-process wait queue woken by the
 * target's own exit, rather than spinning -- that needs parent/child
 * tracking (docs/architecture/process-and-scheduling.md §6.2's `parent`
 * field, Phase D, not built yet). Busy-polling is a correct stand-in until
 * then: process_kill()'s existing "reap the moment it's not current_process"
 * path is what actually clears it out; this just waits for that to happen. */
static int64_t sys_wait(struct regs *r) {
    uint32_t pid = (uint32_t)r->rdi;

    for (;;) {
        struct process *target = process_find(pid);
        if (!target) {
            return -EMBK_ECHILD;
        }
        if (target->state == PROCESS_ZOMBIE) {
            int code = target->exit_code;
            process_reap(pid);
            return code;
        }
        sys_yield();
    }
}

/* getpid() -> this process's pid. Trivial, but a real primitive a spawn()
 * caller needs -- e.g. to tell itself apart from a child that's about to
 * run the exact same binary from the same entry point. */
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
