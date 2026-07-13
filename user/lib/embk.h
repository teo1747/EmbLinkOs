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
/* Surfaces -- shared-memory pixel buffers (EmbLink UI Piece 1). A surface */
/* is a kernel-owned, refcounted run of pages that a UI client and the      */
/* compositor both map (their own VAs, same physical memory) to share       */
/* pixels with zero copies. Named by a typed handle in the process's        */
/* obj_handle table. See kernel/gfx/surface.h.                              */
/* ==================================================================== */

#define EMBK_PIXFMT_BGRA8888_PRE 1   /* premultiplied alpha; only one impl'd */

/* spawn() file-action kind: transfer+map a surface the parent holds into
 * the child (target_fd carries the parent's surface handle). The minimal
 * handle_transfer for Piece 1. */
#define EMBK_SPAWN_ACTION_INHERIT_SURFACE 2

/* Geometry the kernel fills on create/map so both sides agree without
 * re-querying. Mirrors struct surface_info (kernel/gfx/surface.h). */
struct embk_surface_info {
    uint32_t width, height, stride, format, n_buffers;
    uint64_t buffer_size;   /* bytes per buffer; buffer i is at base + i*this */
};

/* Create a surface and map it into THIS process (the client). Returns a
 * surface handle (>= 0) or -EMBK_*. `out` is filled with the geometry. */
static inline int embk_surface_create(uint32_t w, uint32_t h, uint32_t fmt,
                                      uint32_t n_buffers, struct embk_surface_info *out) {
    return (int)embk_syscall5(EMBK_SYS_surface_create, w, h, fmt, n_buffers,
                              (int64_t)(intptr_t)out);
}

/* Map a surface this process holds a handle to (e.g. one inherited at spawn)
 * into this address space. Returns the base VA (buffer 0), or a negative
 * -EMBK_* (test with < 0). Fills `out` with geometry. */
static inline int64_t embk_surface_map(int handle, struct embk_surface_info *out) {
    return embk_syscall2(EMBK_SYS_surface_map, handle, (int64_t)(intptr_t)out);
}

/* Get a buffer index the client may draw into (owner==CLIENT), or
 * -EMBK_EAGAIN if all are held by the compositor (B2). */
static inline int embk_surface_acquire(int handle) {
    return (int)embk_syscall1(EMBK_SYS_surface_acquire, handle);
}
/* Post `idx` as the newest frame (client -> compositor). */
static inline int embk_surface_commit(int handle, int idx) {
    return (int)embk_syscall2(EMBK_SYS_surface_commit, handle, idx);
}
/* Return `idx` to the client (compositor -> client). */
static inline int embk_surface_release(int handle, int idx) {
    return (int)embk_syscall2(EMBK_SYS_surface_release, handle, idx);
}
/* Drop this process's handle + mapping (refcount--; frees at 0). */
static inline int embk_surface_destroy(int handle) {
    return (int)embk_syscall1(EMBK_SYS_surface_destroy, handle);
}

/* Direct present: blit a premultiplied-BGRA8888 pixel buffer (w*h, tight) to
 * the centre of the framebuffer and flush. A minimal single-fullscreen-app
 * path standing in for a compositor. Returns 0 or -EMBK_*. */
static inline int embk_ui_present(const void *pixels, uint32_t w, uint32_t h) {
    return (int)embk_syscall3(EMBK_SYS_ui_present, (int64_t)(intptr_t)pixels, w, h);
}

/* Present only a surface-local sub-rectangle (rx,ry,rw,rh) -- the interactive
 * fast path, so a cursor that moved a few pixels doesn't re-upload the whole
 * surface. Same pixel buffer (w*h tight BGRA8888_PRE) as embk_ui_present. */
static inline int embk_ui_present_rect(const void *pixels, uint32_t w, uint32_t h,
                                       uint32_t rx, uint32_t ry, uint32_t rw, uint32_t rh) {
    int64_t dims = ((int64_t)(w & 0xFFFF) << 16) | (h & 0xFFFF);
    int64_t rect = ((int64_t)(rx & 0xFFFF) << 48) | ((int64_t)(ry & 0xFFFF) << 32)
                 | ((int64_t)(rw & 0xFFFF) << 16) |  (int64_t)(rh & 0xFFFF);
    return (int)embk_syscall3(EMBK_SYS_ui_present_rect, (int64_t)(intptr_t)pixels, dims, rect);
}

/* Pointer (mouse) state for the UI event loop. x/y are screen pixels; buttons
 * is a bitmask of EMBK_MOUSE_*. */
#define EMBK_MOUSE_LEFT   0x01
#define EMBK_MOUSE_RIGHT  0x02
#define EMBK_MOUSE_MIDDLE 0x04
struct embk_ui_input { int32_t x, y; uint32_t buttons; int32_t wheel; };
static inline int embk_ui_input(struct embk_ui_input *out) {
    return (int)embk_syscall1(EMBK_SYS_ui_input, (int64_t)(intptr_t)out);
}

/* Non-blocking keystroke poll for the UI loop: returns the next ASCII byte
 * ('\b'=0x08 backspace, '\n' enter), or 0 when nothing is pending. Drain it in
 * a loop each frame and feed chars to the UI via ui_input_char(). */
static inline int embk_key_poll(void) {
    return (int)embk_syscall0(EMBK_SYS_key_poll);
}

/* Grab (1) or release (0) exclusive keyboard: while grabbed, the kernel shell
 * stops consuming keystrokes so this app gets them all. Auto-released on exit. */
static inline int embk_key_grab(int on) {
    return (int)embk_syscall1(EMBK_SYS_key_grab, on);
}

/* Monotonic milliseconds since boot -- the clock a UI animator ticks on. */
static inline uint64_t embk_uptime_ms(void) {
    return (uint64_t)embk_syscall0(EMBK_SYS_uptime_ms);
}

/* ==================================================================== */
/* Window compositor (EmbLink UI Piece 2). A WINDOW is a positioned tile of */
/* client-rendered pixels the kernel composites over a desktop with a title  */
/* bar and z-order. Unlike embk_ui_present (one centred surface), many        */
/* windows from many processes coexist. Pixels are BGRA8888_PRE (premult),    */
/* same as surfaces: a uint32 is 0xAARRGGBB.                                  */
/* ==================================================================== */

/* Create a window: content `cw` x `ch`, window frame's top-left at screen
 * (x,y) (the title bar occupies the first rows of the frame, content below).
 * Returns a window id (>0) or a negative -errno. */
static inline int embk_win_create(uint32_t cw, uint32_t ch, int32_t x, int32_t y,
                                  const char *title) {
    return (int)embk_syscall5(EMBK_SYS_win_create, cw, ch, x, y,
                              (int64_t)(intptr_t)title);
}

/* Zero-copy window: the kernel maps the window's pixel pages into THIS process
 * and returns the base pointer in *out_pixels. Render directly into it, then
 * call embk_win_present/_rect (which becomes a damage-only call -- no copy).
 * Returns a window id (>0) or a negative -errno. */
static inline int embk_win_create_shared(uint32_t cw, uint32_t ch, int32_t x, int32_t y,
                                         const char *title, void **out_pixels) {
    uint64_t va = 0;
    int id = (int)embk_syscall6(EMBK_SYS_win_create, cw, ch, x, y,
                                (int64_t)(intptr_t)title, (int64_t)(intptr_t)&va);
    if (id >= 0 && out_pixels) *out_pixels = (void *)(intptr_t)va;
    return id;
}

/* Window-style flags for embk_win_create_shared_ex (high bits of the cw arg). */
#define EMBK_WINF_CHROMELESS (1ULL << 32)   /* no kernel bar/close/border: the app
                                             * draws its own chrome (EmUI Window/
                                             * WindowBar) and moves itself. */
#define EMBK_WINF_WIDGET     (1ULL << 33)   /* DESKTOP WIDGET: chromeless AND kept
                                             * in a z-band above the desktop but
                                             * below every app window. */

/* Resize a shared window's content to w x h. The window's pixel pages are
 * REPLACED: *out_pixels receives the NEW mapping base and the old pointer is
 * dead the moment this returns. Returns the window id, or -EMBK_*. */
static inline int embk_win_resize(int id, uint32_t w, uint32_t h, void **out_pixels) {
    uint64_t va = 0;
    int rc = (int)embk_syscall4(EMBK_SYS_win_resize, id, w, h, (int64_t)(intptr_t)&va);
    if (rc >= 0 && out_pixels) *out_pixels = (void *)(intptr_t)va;
    return rc;
}

/* Zero-copy window with style flags. Same as embk_win_create_shared plus
 * EMBK_WINF_* or'd into `flags`. */
static inline int embk_win_create_shared_ex(uint32_t cw, uint32_t ch, int32_t x, int32_t y,
                                            const char *title, uint64_t flags,
                                            void **out_pixels) {
    uint64_t va = 0;
    int id = (int)embk_syscall6(EMBK_SYS_win_create, (int64_t)(flags | cw), ch, x, y,
                                (int64_t)(intptr_t)title, (int64_t)(intptr_t)&va);
    if (id >= 0 && out_pixels) *out_pixels = (void *)(intptr_t)va;
    return id;
}

/* Present the whole content buffer (cw*ch tight BGRA) to window `id`. */
static inline int embk_win_present(int id, const void *pixels,
                                   uint32_t cw, uint32_t ch) {
    int64_t dims = ((int64_t)(cw & 0xFFFF) << 16) | (ch & 0xFFFF);
    return (int)embk_syscall4(EMBK_SYS_win_present, id,
                              (int64_t)(intptr_t)pixels, dims, 0);
}

/* Present only a content-local sub-rectangle (rx,ry,rw,rh) -- the fast path,
 * so a small change doesn't re-upload the whole window. Same cw*ch buffer. */
static inline int embk_win_present_rect(int id, const void *pixels,
                                        uint32_t cw, uint32_t ch,
                                        uint32_t rx, uint32_t ry,
                                        uint32_t rw, uint32_t rh) {
    int64_t dims = ((int64_t)(cw & 0xFFFF) << 16) | (ch & 0xFFFF);
    int64_t rect = ((int64_t)(rx & 0xFFFF) << 48) | ((int64_t)(ry & 0xFFFF) << 32)
                 | ((int64_t)(rw & 0xFFFF) << 16) |  (int64_t)(rh & 0xFFFF);
    return (int)embk_syscall4(EMBK_SYS_win_present, id,
                              (int64_t)(intptr_t)pixels, dims, rect);
}

/* Move a window's frame top-left to screen (x,y); also raises it to the front. */
static inline int embk_win_move(int id, int32_t x, int32_t y) {
    return (int)embk_syscall3(EMBK_SYS_win_move, id, x, y);
}

/* Destroy a window and erase it from the desktop. */
static inline int embk_win_destroy(int id) {
    return (int)embk_syscall1(EMBK_SYS_win_destroy, id);
}

/* Create the full-screen chromeless HOME/desktop window (zero-copy): sized to
 * the framebuffer, pinned at the back, no title bar. The pixel base is returned
 * in out_pixels and the screen size in out_w and out_h. Only one app should hold
 * the desktop (the home launcher). Returns a window id (>0) or a negative -errno. */
static inline int embk_win_create_desktop(void **out_pixels, uint32_t *out_w, uint32_t *out_h) {
    uint64_t va = 0; uint32_t w = 0, h = 0;
    int id = (int)embk_syscall3(EMBK_SYS_win_create_desktop,
                                (int64_t)(intptr_t)&va, (int64_t)(intptr_t)&w,
                                (int64_t)(intptr_t)&h);
    if (id >= 0) {
        if (out_pixels) *out_pixels = (void *)(intptr_t)va;
        if (out_w) *out_w = w;
        if (out_h) *out_h = h;
    }
    return id;
}

/* Sleep at least `ms` milliseconds, YIELDING the CPU the whole time. This is
 * how a UI event loop paces itself -- a volatile spin loop burns the app's
 * whole scheduler slice and starves every other process; this costs ~nothing.
 * Granularity is a scheduler round trip, not a hard timer. */
static inline int embk_sleep_ms(uint64_t ms) {
    return (int)embk_syscall1(EMBK_SYS_sleep_ms, (int64_t)ms);
}

/* 1 if the child named by spawn HANDLE `handle` (what embk_spawn returned) is
 * still alive, else 0 (unknown/freed handles are "not alive"). Handle-based on
 * purpose: spawn never exposes raw pids, and a handle stays pinned to the
 * exact child instance even after pids recycle. Lets a launcher avoid starting
 * a second instance of an app that is still running. */
static inline int embk_proc_alive(int handle) {
    return (int)embk_syscall1(EMBK_SYS_proc_alive, handle);
}

/* Screen (framebuffer) size, so a windowed app can size itself to fit -- e.g.
 * clamp a tall window so it isn't rejected on a short display. */
static inline int embk_screen_size(uint32_t *w, uint32_t *h) {
    return (int)embk_syscall2(EMBK_SYS_screen_size,
                              (int64_t)(intptr_t)w, (int64_t)(intptr_t)h);
}

/* Content-local pointer for a windowed app. `focused` is 1 when the pointer is
 * over THIS process's window content (then x,y are window-local pixels and
 * buttons is the mouse state -- EMBK_MOUSE_LEFT etc.), 0 otherwise. The home
 * launcher reads this to make its tiles clickable. */
struct embk_win_input { int32_t focused; int32_t x, y; uint32_t buttons; uint32_t win; int32_t wheel; };
static inline int embk_win_input(struct embk_win_input *out) {
    return (int)embk_syscall1(EMBK_SYS_win_input, (int64_t)(intptr_t)out);
}

/* ==================================================================== */
/* IPC channels (EmbLink UI Piece 1, Layer A). A channel is a bidirectional */
/* message-oriented endpoint pair. Messages carry a byte payload + 0..N     */
/* ancillary handles, each passed COPY or MOVE. Bulk data (pixels) goes      */
/* through shared surfaces, not payloads -- channels carry control + handles.*/
/* ==================================================================== */

#define EMBK_CHAN_HANDLE_COPY 0   /* sender keeps its handle; receiver gets one too */
#define EMBK_CHAN_HANDLE_MOVE 1   /* sender's handle consumed; capability moves */

/* spawn() file-action: MOVE a channel end the parent holds into the child
 * (target_fd = parent's channel handle). Bootstraps a parent/child channel. */
#define EMBK_SPAWN_ACTION_INHERIT_CHANNEL 3

/* Create a connected pair in THIS process; out[0]/out[1] receive the two end
 * handles. Returns 0 or -EMBK_*. */
static inline int embk_chan_pair(int out_handles[2]) {
    return (int)embk_syscall1(EMBK_SYS_chan_pair, (int64_t)(intptr_t)out_handles);
}

/* Send one message: `len` payload bytes plus `n_hnd` ancillary handles
 * (`hnds[]`) with per-handle COPY/MOVE (`flags[]`, or NULL for all-COPY).
 * Blocks if the peer's inbox is full; -EMBK_EPIPE if the peer has closed.
 * Returns 0 or -EMBK_*. */
static inline int embk_chan_send(int handle, const void *bytes, unsigned len,
                                 const int *hnds, const int *flags, unsigned n_hnd) {
    return (int)embk_syscall6(EMBK_SYS_chan_send, handle, (int64_t)(intptr_t)bytes,
                              len, (int64_t)(intptr_t)hnds, (int64_t)(intptr_t)flags, n_hnd);
}

/* Receive one message. Blocks until one arrives or the peer closes
 * (-EMBK_EPIPE). Fills up to `buflen` payload bytes (-EMBK_EMSGSIZE, message
 * left queued, if it's bigger); *out_len gets the real length. Ancillary
 * handles are installed in this process's table, their ints written to
 * out_hnds[], count to *out_nhnd. Returns 0 or -EMBK_*. */
static inline int embk_chan_recv(int handle, void *buf, unsigned buflen,
                                 unsigned *out_len, int *out_hnds, unsigned *out_nhnd) {
    return (int)embk_syscall6(EMBK_SYS_chan_recv, handle, (int64_t)(intptr_t)buf,
                              buflen, (int64_t)(intptr_t)out_len,
                              (int64_t)(intptr_t)out_hnds, (int64_t)(intptr_t)out_nhnd);
}

/* Close this process's end (wakes the peer with EPIPE). */
static inline int embk_chan_close(int handle) {
    return (int)embk_syscall1(EMBK_SYS_chan_close, handle);
}

/* ==================================================================== */
/* Rendezvous (EmbLink UI Piece 1, Layer B): find a channel peer by a VFS   */
/* path (e.g. "/run/compositor") instead of needing an already-open        */
/* channel or a spawn-time handoff. Backed by a RAM filesystem mounted at   */
/* /run -- a crashed server's path vanishes automatically (kernel/ipc/     */
/* endpoint.h's B4).                                                        */
/* ==================================================================== */

/* Publish a listening endpoint at `path`. Returns an ENDPOINT handle, or
 * -EMBK_EEXIST if the path is already taken. */
static inline int embk_chan_listen(const char *path) {
    return (int)embk_syscall1(EMBK_SYS_chan_listen, (int64_t)(intptr_t)path);
}

/* Block until a client connects; returns a new CHANNEL handle for that
 * connection, or -EMBK_*. */
static inline int embk_chan_accept(int listen_handle) {
    return (int)embk_syscall1(EMBK_SYS_chan_accept, listen_handle);
}

/* Connect to the listener at `path`. Returns a new CHANNEL handle, or
 * -EMBK_ENOENT (no such path) / -EMBK_ECONNREFUSED (path exists but its
 * owner is dead / mid-teardown). */
static inline int embk_chan_connect(const char *path) {
    return (int)embk_syscall1(EMBK_SYS_chan_connect, (int64_t)(intptr_t)path);
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
