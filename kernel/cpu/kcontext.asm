; Minimal setjmp/longjmp implementation for kernel->user->kernel context switching.
; Field order Must match struct kcontext in kernel/cpu/kcontext.h
; rbx=0, rbp=8, r12=16, r13=24, r14=32, r15=40, rsp=48, rip=56
; SysV ABI: 1st arg = rdi (struct kcontext *), 2nd arg = rsi (return value)

global kernel_ctx_save
global kernel_ctx_restore

; uint64_t kernel_ctx_save(struct kcontext *ctx) [ctx in rdi]
kernel_ctx_save:
    mov [rdi + 0],  rbx
    mov [rdi + 8],  rbp
    mov [rdi + 16], r12
    mov [rdi + 24], r13
    mov [rdi + 32], r14
    mov [rdi + 40], r15

    ; RSP as it will be in the caller after we return: current RSP + 8
    ; (pop the return address this caller pushed onto the stack). so 
    ; the restored context resumes with the caller's stackk, not the kernel_ctx_save stack.
    lea rax, [rsp + 8]           ; caller's RSP (skip our return address)
    mov [rdi + 48], rax

    ; RIP to resume at: our return address (sitting at [rsp])
    mov rax, [rsp]               ; resume RIP = our return address
    mov [rdi + 56], rax

    xor eax, eax                 ; the direct call returns 0
    ret

; void kernel_ctx_restore(struct kcontext *ctx, uint64_t ret_val) [ctx in rdi, ret_val in rsi]
kernel_ctx_restore:
    mov rbx, [rdi + 0]
    mov rbp, [rdi + 8]
    mov r12, [rdi + 16]
    mov r13, [rdi + 24]
    mov r14, [rdi + 32]
    mov r15, [rdi + 40]
    mov rsp, [rdi + 48]          ; switch back to the saved kernel stack

    mov rax, rsi                 ; value kernel_ctx_save appears to return    
    test rax, rax                ; if ret_val == 0, we are returning from kernel_ctx_save
    jnz .nonzero_retval          ; if ret_val != 0, we are returning from kernel_ctx_restore
    mov rax, 1                   ; never let restore return 0, would look like direct call
.nonzero_retval:
    mov rcx, [rdi + 56]          ; resume right after kernel_ctx_save
    jmp rcx                      ; jump to the saved RIP