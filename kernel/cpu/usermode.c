/* kernel/cpu/usermode.c */
#include "gdt.h"
#include "kcontext.h"
#include "../mm/vmm.h"
#include "../mm/pmm.h"
#include "../drivers/serial.h"
#include "../cpu/elf.h"
#include "../include/errno.h"
#include "../fs/vfs.h"
#include "../fs/fd.h"
#include "../include/kmalloc.h"



#include <stdint.h>






#define INIT_PROGRAM_PATH "/init.elf"

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


/* Read a static ELF64 executable off the filesystem into a kmalloc'd buffer,
 * then hand it to elf_load — which is source-agnostic, it only ever cared
 * about an image pointer + length. The buffer is freed here, not by elf_load,
 * because by the time elf_load returns it has already COPIED every byte it
 * needs into user pages (the p_filesz copy loop runs synchronously inside
 * it) — nothing downstream reads from this buffer again. */
static int elf_load_from_file(const char *path, uint64_t *entry_out)
{
    int fd = vfs_open(path, O_RDONLY, 0);
    if (fd < 0) {
        serial_write_string("elf_load_from_file: open failed: ");
        serial_write_hex((uint64_t)(-fd));
        serial_write_string("\n");
        return fd;
    }

    struct vfs_stat st;
    int rc = vfs_fd_fstat(fd, &st);
    if (rc != EMBK_OK) { vfs_close(fd); return rc; }
    if (st.size == 0)  { vfs_close(fd); return -EMBK_EINVAL; }

    uint8_t *buf = kmalloc(st.size);
    if (!buf) { vfs_close(fd); return -EMBK_ENOMEM; }

    /* Robust read loop: ONE vfs_fd_read call is not guaranteed to return the
     * whole file. Keep reading until we have st.size bytes, or hit EOF short
     * of that (truncated/corrupt — do not hand a partial image to the ELF
     * parser), or a hard error. */
    uint64_t total = 0;
    while (total < st.size) {
        size_t got = 0;
        rc = vfs_fd_read(fd, buf + total, (size_t)(st.size - total), &got);
        if (rc != EMBK_OK) { kfree(buf); vfs_close(fd); return rc; }
        if (got == 0)      { kfree(buf); vfs_close(fd); return -EMBK_EIO; }
        total += got;
    }
    vfs_close(fd);   /* done with the fd; only the buffer matters now */

    rc = elf_load(buf, total, entry_out);
    kfree(buf);
    return rc;
}


void enter_user_mode(void)
{
    uint64_t entry = 0;
    int rc = elf_load_from_file(INIT_PROGRAM_PATH, &entry);
    if (rc != EMBK_OK) {
        serial_write_string("Failed to load init program: ");
        serial_write_hex((uint64_t)(-rc));
        serial_write_string("\n");
        return;
    }

    /* everything below is UNCHANGED: user stack, kernel_ctx_save, iretq */
    uint64_t stack_phys = pmm_alloc_page();
    vmm_map(USER_STACK_VA, stack_phys, VMM_USER | VMM_WRITABLE | VMM_NX);
    uint64_t user_rsp = USER_STACK_VA + PAGE_SIZE_4K;

    if (kernel_ctx_save(&g_user_exit_ctx) != 0) {
        serial_write_string("Returned to kernel: user program exited.\n");
        return;
    }

    serial_write_string("Entering ring 3 at entry=");
    serial_write_hex(entry);
    serial_write_string("\n");

    __asm__ volatile (
        "pushq %0\n" "pushq %1\n" "pushq $0x202\n" "pushq %2\n" "pushq %3\n"
        "iretq\n"
        : : "r"((uint64_t)(0x18|3)), "r"(user_rsp),
            "r"((uint64_t)(0x20|3)), "r"(entry)
        : "memory"
    );
}