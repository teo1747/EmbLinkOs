#ifndef __SYSCALL_H__
#define __SYSCALL_H__

#include <stdint.h>

struct regs {
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t r11;
    uint64_t r10;
    uint64_t r9;
    uint64_t r8;
    // Order MUST match the push sequence in syscall_entry.asm:
    //   push rsi; push rdi; push rbp  ->  low-to-high on the stack: rbp, rdi, rsi
    // The previous rsi/rbp swap made sys_write read rbp as the user buffer.
    uint64_t rbp;
    uint64_t rdi;
    uint64_t rsi;
    uint64_t rdx;
    uint64_t rcx;
    uint64_t rbx;
    uint64_t rax;

    // Interrupt number and error code (if applicable)
    uint64_t int_no;
    uint64_t err_code;

    // Pushed by the processor automatically
    uint64_t rip;
    uint64_t cs;
    uint64_t eflags;
    uint64_t rsp;
    uint64_t ss;
};


/* Defined in syscall.c, called from syscall_entry.asm. */
void syscall_dispatch(struct regs *r);

void syscall_init(void);

#endif /* __SYSCALL_H__ */