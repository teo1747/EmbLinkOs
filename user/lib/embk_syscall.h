/* user/embk_syscall.h — the raw EmbLink OS system-call ABI, shared by every
 * userland translation unit (the newlib retargeting layer user/syscalls.c,
 * and any freestanding program that wants to bypass libc). This is the ONE
 * place the syscall numbers and the int-0x80 register convention live on the
 * user side.
 *
 * THESE NUMBERS MUST MATCH kernel/cpu/syscall.c's SYS_* table exactly. There
 * is no shared kernel/user header for them yet (the kernel keeps its own
 * copy), so this is hand-synchronized -- adding or renumbering a syscall
 * means editing both. A mismatch fails silently (wrong handler runs), so
 * treat this list as load-bearing. */

#ifndef __EMBK_SYSCALL_H__
#define __EMBK_SYSCALL_H__

#include <stdint.h>
#include <stddef.h>

/* --- syscall numbers (rax) -- keep in lockstep with kernel/cpu/syscall.c --- */
#define EMBK_SYS_write          1
#define EMBK_SYS_exit           2
#define EMBK_SYS_yield          3
#define EMBK_SYS_open           4
#define EMBK_SYS_close          5
#define EMBK_SYS_read           6
#define EMBK_SYS_lseek          7
#define EMBK_SYS_stat           8
#define EMBK_SYS_readdir        9
#define EMBK_SYS_spawn          10
#define EMBK_SYS_wait           11
#define EMBK_SYS_getpid         12
#define EMBK_SYS_kill           13
#define EMBK_SYS_thread_create  14
#define EMBK_SYS_thread_join    15
#define EMBK_SYS_thread_exit    16
#define EMBK_SYS_sbrk           17
#define EMBK_SYS_fstat          18
#define EMBK_SYS_gettimeofday   19
#define EMBK_SYS_surface_create   20
#define EMBK_SYS_surface_map      21
#define EMBK_SYS_surface_acquire  22
#define EMBK_SYS_surface_commit   23
#define EMBK_SYS_surface_release  24
#define EMBK_SYS_surface_destroy  25
#define EMBK_SYS_chan_pair    26
#define EMBK_SYS_chan_send    27
#define EMBK_SYS_chan_recv    28
#define EMBK_SYS_chan_close   29
#define EMBK_SYS_chan_listen  30
#define EMBK_SYS_chan_accept  31
#define EMBK_SYS_chan_connect 32
#define EMBK_SYS_ui_present   33
#define EMBK_SYS_ui_input     34
#define EMBK_SYS_ui_present_rect 35
#define EMBK_SYS_key_poll     36
#define EMBK_SYS_key_grab     37
#define EMBK_SYS_uptime_ms    38
#define EMBK_SYS_win_create   39
#define EMBK_SYS_win_present  40
#define EMBK_SYS_win_move     41
#define EMBK_SYS_win_destroy  42
#define EMBK_SYS_win_create_desktop 43
#define EMBK_SYS_win_input    44
#define EMBK_SYS_screen_size  45
#define EMBK_SYS_sleep_ms     46
#define EMBK_SYS_proc_alive   47
#define EMBK_SYS_win_resize   48

/* The raw int-0x80 register convention (mirrors kernel/cpu/syscall_entry.asm
 * + struct regs): number in rax, args in rdi, rsi, rdx, r10, r8; result back
 * in rax. r10/r8 (not rcx/r9) for args 4/5 -- rcx and r11 are clobbered by
 * the syscall-entry path, exactly like the Linux x86-64 int-0x80/syscall
 * convention this deliberately echoes. Every wrapper returns int64_t so the
 * kernel's negative -EMBK_* error codes survive sign-extension intact. */

static inline int64_t embk_syscall0(int64_t n) {
    int64_t ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(n) : "rcx", "r11", "memory");
    return ret;
}

static inline int64_t embk_syscall1(int64_t n, int64_t a1) {
    int64_t ret;
    __asm__ volatile ("int $0x80" : "=a"(ret)
        : "a"(n), "D"(a1) : "rcx", "r11", "memory");
    return ret;
}

static inline int64_t embk_syscall2(int64_t n, int64_t a1, int64_t a2) {
    int64_t ret;
    __asm__ volatile ("int $0x80" : "=a"(ret)
        : "a"(n), "D"(a1), "S"(a2) : "rcx", "r11", "memory");
    return ret;
}

static inline int64_t embk_syscall3(int64_t n, int64_t a1, int64_t a2, int64_t a3) {
    int64_t ret;
    __asm__ volatile ("int $0x80" : "=a"(ret)
        : "a"(n), "D"(a1), "S"(a2), "d"(a3) : "rcx", "r11", "memory");
    return ret;
}

static inline int64_t embk_syscall4(int64_t n, int64_t a1, int64_t a2, int64_t a3,
                                     int64_t a4) {
    int64_t ret;
    register int64_t r10 __asm__("r10") = a4;
    __asm__ volatile ("int $0x80" : "=a"(ret)
        : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10)
        : "rcx", "r11", "memory");
    return ret;
}

static inline int64_t embk_syscall5(int64_t n, int64_t a1, int64_t a2, int64_t a3,
                                     int64_t a4, int64_t a5) {
    int64_t ret;
    register int64_t r10 __asm__("r10") = a4;
    register int64_t r8  __asm__("r8")  = a5;
    __asm__ volatile ("int $0x80" : "=a"(ret)
        : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8)
        : "rcx", "r11", "memory");
    return ret;
}

static inline int64_t embk_syscall6(int64_t n, int64_t a1, int64_t a2, int64_t a3,
                                     int64_t a4, int64_t a5, int64_t a6) {
    int64_t ret;
    register int64_t r10 __asm__("r10") = a4;
    register int64_t r8  __asm__("r8")  = a5;
    register int64_t r9  __asm__("r9")  = a6;
    __asm__ volatile ("int $0x80" : "=a"(ret)
        : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory");
    return ret;
}

/* The kernel returns a small negative value (-EMBK_*, all < 4096 in
 * magnitude) on failure and a normal value (fd, byte count, address, pid,
 * 0) otherwise. This is exactly the Linux-style "errno range" convention:
 * a return in [-4095, -1] is an error code, everything else is a real
 * result -- which works uniformly for fds (small +), sizes (+), sbrk
 * addresses (large +, never in the error window), and pids. The newlib
 * stubs (user/syscalls.c) use this to decide when to set errno + return -1. */
static inline int embk_is_err(int64_t ret) {
    return ret < 0 && ret >= -4095;
}

#endif /* __EMBK_SYSCALL_H__ */
