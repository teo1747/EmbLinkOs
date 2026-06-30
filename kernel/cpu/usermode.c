/* kernel/cpu/usermode.c */
#include "gdt.h"
#include "kcontext.h"
#include "../mm/vmm.h"
#include "../mm/pmm.h"
#include "../drivers/serial.h"
#include <stdint.h>



/* The embedded user program, bracketed by labels from init_blob.asm. Declared
 * as arrays so the names decay to the linker symbols' ADDRESSES; end - start is
 * therefore the byte length. */
extern const uint8_t init_blob_start[];
extern const uint8_t init_blob_end[];
#define INIT_BLOB_LEN ((uint64_t)(init_blob_end - init_blob_start))


/* Saved kernel context to resume when the ring-3 task exits. sys_exit
 * (syscall.c) restores this instead of halting, so control returns here. */
struct kcontext g_user_exit_ctx;

#define USER_CODE_SEL  (0x20 | 3)   /* -> 0x23, RPL 3 */
#define USER_DATA_SEL  (0x18 | 3)   /* -> 0x1B, RPL 3 */

#define USER_CODE_VA   0x0000400000000000ULL   /* low-half: user space   */
#define USER_STACK_VA  0x0000700000000000ULL


#define USER_LOAD_BASE   0x400000ULL          /* MUST equal user.ld's base */
#define PAGE_SIZE_4K     0x1000ULL

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


void enter_user_mode(void)
{
    uint64_t blob_len = INIT_BLOB_LEN;
    serial_write_string("Loading user program, bytes=");
    serial_write_hex(blob_len);
    serial_write_string("\n");

    /* 1. Map enough user pages at the link base to hold the blob, and copy it
     *    in. v1: one RWX region (W^X deferred to the ELF loader — see TODO).
     *    Map page-by-page so a multi-page program just works. */
    uint64_t pages = (blob_len + PAGE_SIZE_4K - 1) / PAGE_SIZE_4K;
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t va   = USER_LOAD_BASE + i * PAGE_SIZE_4K;
        uint64_t phys = pmm_alloc_page();
        if (!phys) {
            serial_write_string("FATAL: out of frames loading user program\n");
            for (;;) __asm__ volatile("cli; hlt");
        }
        /* WRITABLE so the copy below lands; USER so ring 3 can fetch it.
         * (RWX for now — code+data share the region.) */
        vmm_map(va, phys, VMM_WRITABLE | VMM_USER);
    }

    /* Copy the blob into the freshly mapped user VA. After vmm_map, the user
     * VA is valid in THIS address space, so a plain byte copy works. */
    volatile uint8_t *dst = (volatile uint8_t *)USER_LOAD_BASE;
    for (uint64_t i = 0; i < blob_len; i++)
        dst[i] = init_blob_start[i];

    /* 2. A user stack page. */
    uint64_t stack_phys = pmm_alloc_page();
    vmm_map(USER_STACK_VA, stack_phys, VMM_WRITABLE | VMM_USER);
    uint64_t user_rsp = USER_STACK_VA + PAGE_SIZE_4K;     /* top, grows down */


    /* Save the kernel context. Direct call returns 0 -> we proceed to drop
     * into ring 3. When sys_exit calls kernel_ctx_restore(&g_user_exit_ctx, 1),
     * execution reappears HERE returning 1, and we skip the iretq and fall
     * through back into the kernel. */
    if (kernel_ctx_save(&g_user_exit_ctx) != 0) {
        /* We got here via kernel_ctx_restore from sys_exit, NOT via iret, so the
         * int 0x80 interrupt gate's cleared IF was never restored. Re-enable
         * interrupts or IRQs (keyboard, timer) stay masked forever. */
        
        serial_write_string("Returned to kernel: user program exited.\n");
        return;                       /* clean return into the kernel */
    }

    serial_write_string("Entering ring 3 at 0x400000...\n");
    __asm__ volatile (
        "pushq %0\n" "pushq %1\n" "pushq $0x202\n" "pushq %2\n" "pushq %3\n"
        "iretq\n"
        : : "r"((uint64_t)(0x18|3)), "r"(user_rsp),
            "r"((uint64_t)(0x20|3)), "r"(USER_LOAD_BASE)
        : "memory"
    );
}