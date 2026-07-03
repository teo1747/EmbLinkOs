#include "syscall.h"
#include "idt.h"
#include "kcontext.h"
#include "../drivers/serial.h"
#include "../include/errno.h"
#include "../include/types.h"

/* Saved kernel context to resume on exit(), defined in usermode.c. */
extern struct kcontext g_user_exit_ctx;



/* --- Individual syscall handlers --- */

/* write(fd, buf, len) -> bytes written . v1: serial only
 * SECURITY NOTE: buf is a USER pointer. For this proof we trust it; a real
 * Kernel must validate it points into user space and is mapped  before
 * dereferencing (a user passing a kernel address here would otherwise read/write kernel memory)
 * That check arrives with per-process address space management */
static int64_t sys_write(struct regs *r) {
    int fd = (int)r->rdi;
    const char *buf = (const char *)r->rsi;
    size_t len = (size_t)r->rdx;

    if (fd == 1) { // stdout
        for (size_t i = 0; i < len; i++) {
            serial_write_char(buf[i]);
        }
        return (int64_t)len;
    } else {
        return -EMBK_EINVAL; // Invalid file descriptor
    }
}

/* read(fd, buf, len) -> bytes read. v1: serial only
 * SECURITY NOTE: buf is a USER pointer. For this proof we trust it; a real
 * Kernel must validate it points into user space and is mapped  before
 * dereferencing (a user passing a kernel address here would otherwise read/write kernel memory)
 * That check arrives with per-process address space management */
//static int64_t sys_read(struct regs *r) 


/* exit(code): for now, just announce and halt. Becomes "end this process and
 * schedule another" onnce processes exist. */
static int64_t sys_exit(struct regs *r) {
    int code = (int)r->rdi;
    serial_write_string("\n[syscall] exit code=");
    serial_write_hex(code);
    serial_write_string("\n");

    /* No scheduler yet: unwind back into enter_user_mode (which saved this
     * context before dropping to ring 3) instead of halting. This abandons the
     * current int 0x80 frame on the kernel (TSS) stack, which is fine — the next
     * syscall starts fresh from RSP0. Does not return. Once processes exist this
     * becomes "tear down this process and schedule another". */
    kernel_ctx_restore(&g_user_exit_ctx, 1);

    return 0; // never reached
}



/* --- The table: index = syscall number --- */
typedef int64_t (*syscall_handler_t)(struct regs *);

#define SYS_write 1
#define SYS_exit  2


static syscall_handler_t syscall_table[] = {
    [SYS_write] = sys_write,
    [SYS_exit]  = sys_exit,
};

#define SYSCALL_TABLE_SIZE (sizeof(syscall_table) / sizeof(syscall_handler_t))

/* Called from the asm stub (syscall_entry.asm) with a pointer to the saved
 * register frame. Must have external linkage so the assembler's
 * `extern syscall_dispatch` resolves — a `static` here would not link.
 * Number in rax; result written back into rax (the stub pops it to the user).*/
void syscall_dispatch(struct regs *r) {
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
