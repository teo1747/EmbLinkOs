#ifndef __FD_H__
#define __FD_H__

#include "include/types.h"
#include "fs/vfs.h"
#include <stdint.h>

/* Opaque forward declaration -- fd.h can't include process.h (process.h
 * itself includes fd.h, to embed struct fd_entry fds[] in struct process;
 * including it back here would be circular). Without this, every TU that
 * uses fd_open_into()'s prototype below invents its OWN anonymous, mutually
 * incompatible "struct process" (GCC's "declared inside parameter list"
 * warning) -- harmless for a pointer that's only ever passed through, until
 * fd.c itself both sees this prototype AND includes the REAL process.h,
 * at which point the two disagree and fd_open_into()'s definition fails to
 * compile ("conflicting types"). One forward declaration here makes every
 * TU's `struct process *` the same (opaque) type, completed later wherever
 * process.h is actually included. */
struct process;

/* open() flags. bit values are our own; only O_CREAT/O_EXCL act in v1.
 * O_TRUNC and O_APPEND are reserved - declared so callers can compile against
 * the final ABI, but the open/write path don't use them yet*/

#define O_RDONLY     0x0000
#define O_WRONLY     0x0001
#define O_RDWR       0x0002
#define O_ACCMODE    0x0003  /* mask for the above three */
#define O_CREAT      0x0040
#define O_EXCL       0x0080
#define O_TRUNC      0x0200  /* reserved (needs a truncate op)*/
#define O_APPEND     0x0400  /* reserved  (write is cursor-driven for now)*/

/* Fds 0/1/2 are reserved for stdin, stdout, stderr, so returned fds start at
 * FD_BASE and map to slot (fd - FD_BASE). Public (not fd.c-local) because
 * struct process (process.h) embeds a table of these -- each process gets
 * its own fd namespace rather than sharing one global table. */
#define FD_BASE 0        /* was 3. Fds 0/1/2 are now REAL table slots. */
#define FD_STDIO_MAX 3   /* slots 0,1,2 are reserved for stdin/stdout/stderr;
                          * vfs_open's free-slot scan starts ABOVE them, so a
                          * fresh open() can never be handed fd 0 and later
                          * collide with a stdio write. */
#define FD_MAX_OPEN 64

/* What backs an open fd. The per-backing behaviour is a struct fd_ops table
 * (defined in fd.c); fd_entry only holds a pointer to it. */
enum fd_backing {
    FD_BACKING_NONE = 0,      /**< free / not backed */
    FD_BACKING_VNODE,         /**< a VFS file */
    FD_BACKING_CONSOLE,       /**< the kernel console (keyboard in, kputchar out) */
    FD_BACKING_PIPE,          /**< one end of a kernel pipe (kernel/ipc/pipe.c) */
};

struct pipe;   /* opaque -- defined in kernel/ipc/pipe.c; the fd layer only
                * stores the pointer + side and dispatches through pipe_fd_ops. */

/* Per-backing dispatch, sme pattern as struct vfs_ops and struct gpu_driver.
 * Adding a fourth backing later touches this table, not four switch statements. 
 * Signatures deliberately MATCH the existing vfs_fd_* out-param style.
 * (out_read/out_written/out_offset) rather than returning ssize_t -- so the
 * vnode arm is a straight move of the code that's already there and already
 * correct, not a rewrite. */
struct fd_entry;   /* completed below; fd_ops' fn-ptrs take it by pointer, so a
                    * forward decl here keeps every including TU agreeing on the
                    * type (without it GCC warns "declared inside parameter list"
                    * and scopes a phantom fd_entry to each prototype). */
struct fd_ops {
    int  (*read)(struct fd_entry *e, void *buf, size_t len, size_t *out_read);
    int  (*write)(struct fd_entry *e, const void *buf, size_t len, size_t *out_written);
    int  (*seek)(struct fd_entry *e, int64_t delta, int whence, uint64_t *out_offset);
    int  (*fstat)(struct fd_entry *e, struct vfs_stat *out);

    /* Populate `dst` as a child's inherited copy of `src`. Per-backing
     * because "inherit" means genuinely different things: a stateless
     * singleton is a struct copy; a refcounted object needs its count
     * bumped. Getting this wrong is a silent UAF, so it's a dispatch, not
     * a memcpy. */
    int  (*inherit)(struct fd_entry *dst, const struct fd_entry *src);

    /* Release whatever this entry holds, WITHOUT clearing the entry itself
     * (the caller does that). Exists because vfs_close currently hardcodes
     * a vnode obj_put inline -- once an fd can hold a pipe, that assumption
     * is wrong, and once an fd can be REPLACED in-place (a redirect landing
     * on an inherited stdio slot), the release has to happen there too. */
    void (*close)(struct fd_entry *e);

    /* like close(), but assumes the caller already holds g_sched_lock. Needed
     * by obj_handles_release_all() (process.c) which is ALWAYS called with
     * that lock held, and which can be called from process_reap_slot() while
     * running R2 cleanup. */
    void (*close_locked)(struct fd_entry *e);
};

struct fd_entry {
    bool used;
    enum fd_backing backing;    /**< selects which fd_ops table applies */
    const struct fd_ops *ops;   /**< per-backing dispatch (NULL when unused) */
    int flags;
    union {
        struct { struct vnode vn; uint64_t pos; } file;  /**< FD_BACKING_VNODE */
        struct { struct pipe *p; int side; } pipe;       /**< FD_BACKING_PIPE (side: 0=read, 1=write) */
        /* FD_BACKING_CONSOLE is stateless -- no arm needed. */
    } u;
};

void vfs_fd_init(void);

/* The Unix-shared fd surfaces, by path. returns an fd >= 3 on success, or a
 * negative error code on failure. */

int vfs_open(const char *path, int flags, uint32_t mode);
int vfs_close(int fd);
int vfs_fd_read(int fd, void *buf, size_t len, size_t *out_read);
int vfs_fd_write(int fd, const void *buf, size_t len, size_t *out_written);
int vfs_fd_seek(int fd, int64_t delta, int whence, uint64_t *out_offset);
int vfs_fd_fstat(int fd, struct vfs_stat *out);
int fd_open_into(struct process *target, int target_fd, const char *path, int flags, uint32_t mode);

/* Give a new process its stdin/stdout/stderr (fds 0/1/2): inherit from the
 * spawning parent per-backing, or default to the console. process_create()
 * calls this BEFORE applying file actions. */
void fds_init_stdio(struct process *proc);
int vfs_fd_run_selftests(void);


#endif /* __FD_H__ */