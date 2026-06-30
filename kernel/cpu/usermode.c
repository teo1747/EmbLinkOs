/* kernel/cpu/usermode.c */
#include "gdt.h"
#include "kcontext.h"
#include "../mm/vmm.h"
#include "../mm/pmm.h"
#include "../drivers/serial.h"
#include <stdint.h>

/* Saved kernel context to resume when the ring-3 task exits. sys_exit
 * (syscall.c) restores this instead of halting, so control returns here. */
struct kcontext g_user_exit_ctx;

#define USER_CODE_SEL  (0x20 | 3)   /* -> 0x23, RPL 3 */
#define USER_DATA_SEL  (0x18 | 3)   /* -> 0x1B, RPL 3 */

#define USER_CODE_VA   0x0000400000000000ULL   /* low-half: user space   */
#define USER_STACK_VA  0x0000700000000000ULL

/* the new user_stub — runs in ring 3, calls write then exit.
 *
 * This payload is COPIED to USER_CODE_VA (see enter_user_mode) and run there,
 * so it must be position-independent: no RIP-relative data references, because
 * the copy runs at a different address than where it was linked. That is why
 * the message pointer is loaded with `movabs` (an absolute imm64 that survives
 * the relocation) instead of `lea msg(%rip)` (which would point into garbage
 * after the copy). msg stays in the kernel's rodata; sys_write runs in ring 0
 * and can read that address. (A real kernel would copy the string into user
 * memory and validate the pointer — see the SECURITY NOTE in syscall.c.) */
__attribute__((noinline, used))
static void user_stub(void)
{
    static const char msg[] = "Hello from ring 3 via syscall!\n";

    /* write(fd=1, buf=msg, len=...) : number in rax, args rdi/rsi/rdx */
    __asm__ volatile (
        "mov $1, %%rax\n"          /* SYS_write */
        "mov $1, %%rdi\n"          /* fd = 1    */
        "movabs %[m], %%rsi\n"     /* buf (absolute addr, relocation-safe) */
        "mov %[n], %%rdx\n"        /* len       */
        "int $0x80\n"
        :
        : [m] "i"(&msg[0]), [n] "i"(sizeof msg - 1)
        : "rax", "rdi", "rsi", "rdx", "rcx", "r11", "memory"
    );

    /* exit(0) */
    __asm__ volatile (
        "mov $2, %%rax\n"          /* SYS_exit */
        "mov $0, %%rdi\n"
        "int $0x80\n"
        : : : "rax", "rdi", "memory"
    );
}

void enter_user_mode(void) {
    /* 0. Save a kernel context to come back to. On the direct call this returns
     * 0 and we fall through to ring 3. When the ring-3 task calls exit(),
     * sys_exit does kernel_ctx_restore(&g_user_exit_ctx, 1), which resumes here
     * with a nonzero return — we re-enable interrupts (the int 0x80 gate cleared
     * IF) and return to the caller so the shell runs. */
    if (kernel_ctx_save(&g_user_exit_ctx) != 0) {
        __asm__ volatile ("sti");
        serial_write_string("Returned to kernel from ring 3.\n");
        return;
    }

    /* 1. Fresh user code page, copy the stub's bytes in. */
    uint64_t code_phys = pmm_alloc_page();
    vmm_map(USER_CODE_VA, code_phys, VMM_USER);          /* exec, RO, U/S=1 */

    volatile uint8_t *dst = (volatile uint8_t *)USER_CODE_VA;
    const uint8_t *src = (const uint8_t *)&user_stub;
    for (int i = 0; i < 64; i++)                          /* 64 B >> the stub */
        dst[i] = src[i];

    /* 2. Fresh user stack page. */
    uint64_t stack_phys = pmm_alloc_page();
    vmm_map(USER_STACK_VA, stack_phys, VMM_WRITABLE | VMM_USER);
    uint64_t user_rsp = USER_STACK_VA + 0x1000;           /* top, grows down */

    serial_write_string("Entering ring 3...\n");

    /* 3. Synthesize the interrupt frame and iretq into ring 3.
     * Pop order: RIP, CS, RFLAGS, RSP, SS  ->  push in reverse. */
    __asm__ volatile (
        "pushq %0\n"        /* SS     = user data | 3   */
        "pushq %1\n"        /* RSP    = user stack top  */
        "pushq $0x202\n"    /* RFLAGS = IF | reserved-1 */
        "pushq %2\n"        /* CS     = user code | 3   */
        "pushq %3\n"        /* RIP    = user code VA    */
        "iretq\n"
        :
        : "r"((uint64_t)USER_DATA_SEL),
          "r"(user_rsp),
          "r"((uint64_t)USER_CODE_SEL),
          "r"(USER_CODE_VA)        /* enter the COPY, not &user_stub */
        : "memory"
    );
}