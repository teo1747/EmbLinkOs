/* kernel/cpu/usermode.c */
#include "drivers/char/serial.h"
#include "process/process.h"

#include <stdint.h>

#define INIT_PROGRAM_PATH "/init.elf"

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




/* Launches INIT_PROGRAM_PATH and blocks until it exits.
 *
 * Used to hand-roll its own address space + ELF load + raw iretq, with a
 * kernel_ctx_save()/kernel_ctx_restore() pair standing in for "return here
 * when the ring-3 task exits". That model predates the scheduler: it never
 * registered a struct thread/process for the launched program at all, so
 * current_thread stayed whatever the CALLER already was (the shell itself,
 * adopted via process_adopt_current() in main.c) for the program's entire
 * run. Once sys_exit() was unified onto the scheduler-driven
 * process_exit_self() (which zombies current_thread unconditionally), that
 * stopped being harmless: the launched program's own exit() zombied and
 * reaped the SHELL's thread instead of its own, hanging the kernel (the
 * shell never came back). process_create() + process_wait() is the exact
 * "launch as a real scheduled process, block the caller until it exits"
 * machinery "run <path>" (main.c) and "test ring3 threads" (selftests.c)
 * already use correctly -- reused here instead of re-deriving it. */
void enter_user_mode(void)
{
    char *argv[] = { (char *)INIT_PROGRAM_PATH, NULL };
    int pid = process_create(INIT_PROGRAM_PATH, argv, 1, NULL, 0);
    if (pid < 0) {
        serial_write_string("enter_user_mode: process_create failed: ");
        serial_write_hex((uint64_t)(uint32_t)(-pid));
        serial_write_string("\n");
        return;
    }

    serial_write_string("Entering ring 3 (pid=");
    serial_write_hex((uint64_t)(uint32_t)pid);
    serial_write_string(")\n");

    int exit_code = process_wait((uint32_t)pid);
    serial_write_string("Returned to kernel: user program exited, code=");
    serial_write_hex((uint64_t)(uint32_t)exit_code);
    serial_write_string("\n");
}