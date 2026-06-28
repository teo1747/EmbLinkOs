/* kernel/cpu/usermode.c */
#include "gdt.h"
#include "../mm/vmm.h"
#include "../mm/pmm.h"
#include "../drivers/serial.h"
#include <stdint.h>

#define USER_CODE_SEL  (0x20 | 3)   /* -> 0x23, RPL 3 */
#define USER_DATA_SEL  (0x18 | 3)   /* -> 0x1B, RPL 3 */

#define USER_CODE_VA   0x0000400000000000ULL   /* low-half: user space   */
#define USER_STACK_VA  0x0000700000000000ULL

/* The ring-3 payload. Runs a privileged instruction (cli) on purpose: illegal
 * at CPL 3 -> #GP. Kept tiny and position-independent-ish so copying its bytes
 * to another page still runs. We copy a fixed byte count below. */
__attribute__((noinline, used))
static void user_stub(void) {
    __asm__ volatile ("cli");      /* faults here, from ring 3 */
    for (;;) { __asm__ volatile ("pause"); }
}

void enter_user_mode(void) {
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