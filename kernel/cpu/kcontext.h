#ifndef __KCONTEXT_H__
#define __KCONTEXT_H__

#include <stdint.h>

// A minimal saved kernel execution context (setjmp/longjmp style). With no
// scheduler yet, this is how a ring-3 task that calls exit() unwinds back into
// the kernel instead of halting: enter_user_mode saves a context, sys_exit
// restores it. Only the callee-saved registers, the stack pointer, and the
// resume address need saving. Field order MUST match the offsets used by
// kernel_ctx_save / kernel_ctx_restore in kcontext.asm.
struct kcontext {
    uint64_t rbx, rbp, r12, r13, r14, r15, rsp, rip, rflags;
};

// setjmp: save the current context into ctx. Returns 0 on the direct call, and
// the (nonzero) value passed to kernel_ctx_restore when resumed.
uint64_t kernel_ctx_save(struct kcontext *ctx);

// longjmp: restore ctx so the matching kernel_ctx_save appears to return `val`
// (pass a nonzero value). Does not return to its own caller.
void kernel_ctx_restore(struct kcontext *ctx, uint64_t val);

#endif /* __KCONTEXT_H__ */
