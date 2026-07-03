/* kernel/cpu/usermode.c */
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
static int elf_load_from_file(const char *path, uint64_t pml4_phys,
                              uint64_t *entry_out)
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

    uint64_t total = 0;
    while (total < st.size) {
        size_t got = 0;
        rc = vfs_fd_read(fd, buf + total, (size_t)(st.size - total), &got);
        if (rc != EMBK_OK) { kfree(buf); vfs_close(fd); return rc; }
        if (got == 0)      { kfree(buf); vfs_close(fd); return -EMBK_EIO; }
        total += got;
    }
    vfs_close(fd);

    /* Hand the in-memory image to the parser, targeting the PROCESS address
     * space — elf_load maps each segment into pml4_phys, not the kernel PML4. */
    rc = elf_load(buf, total, pml4_phys, entry_out);
    kfree(buf);
    return rc;
}

void enter_user_mode(void)
{
    /* 1. Create the process address space (kernel half aliased, user half empty). */
    uint64_t proc_pml4 = vmm_create_address_space();
    if (!proc_pml4) {
        serial_write_string("enter_user_mode: no memory for address space\n");
        return;
    }

    /* 2. Load the ELF INTO that address space. elf_load must map via
     *    vmm_map_in(proc_pml4, ...) now, not the kernel PML4 — so it needs the
     *    target pml4 passed in. (See note below.) */
    uint64_t entry = 0;
    int rc = elf_load_from_file(INIT_PROGRAM_PATH, proc_pml4, &entry);
    if (rc != EMBK_OK) {
        serial_write_string("enter_user_mode: ELF load failed: ");
        serial_write_hex((uint64_t)(-rc));
        serial_write_string("\n");
        vmm_destroy_address_space(proc_pml4);
        return;
    }

    /* 3. Map the user stack into the SAME process address space. */
    uint64_t stack_phys = pmm_alloc_page();
    if (!stack_phys) {
        serial_write_string("no stack frame\n");
        vmm_destroy_address_space(proc_pml4);
        return;
    }
    if (vmm_map_in(proc_pml4, USER_STACK_VA, stack_phys,
                   VMM_USER | VMM_WRITABLE | VMM_NX) < 0) {
        serial_write_string("enter_user_mode: stack map failed\n");
        pmm_free_page(stack_phys);
        vmm_destroy_address_space(proc_pml4);
        return;
    }
    uint64_t user_rsp = USER_STACK_VA + PAGE_SIZE_4K;

    /* Save the kernel return context (unchanged — this stack is higher-half,
     * survives the CR3 switch). */
    if (kernel_ctx_save(&g_user_exit_ctx) != 0) {
        serial_write_string("Returned to kernel: user program exited.\n");
        vmm_switch_address_space(vmm_get_kernel_pml4());
        vmm_destroy_address_space(proc_pml4);
        return;
    }

    serial_write_string("Entering ring 3 at entry=");
    serial_write_hex(entry);
    serial_write_string("\n");

    /* 4. Switch CR3 to the process address space — LATE, right before iretq. */
    vmm_switch_address_space(proc_pml4);

    /* 5. iretq into ring 3. RIP (entry) and RSP (user_rsp) are process-private
     *    lower-half addresses that exist only in proc_pml4 — which is why the
     *    switch had to happen first. */
    __asm__ volatile (
        "pushq %0\n" "pushq %1\n" "pushq $0x202\n" "pushq %2\n" "pushq %3\n"
        "iretq\n"
        : : "r"((uint64_t)(0x18|3)), "r"(user_rsp),
            "r"((uint64_t)(0x20|3)), "r"(entry)
        : "memory"
    );
}