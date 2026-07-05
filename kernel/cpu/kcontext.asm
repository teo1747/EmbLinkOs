; Minimal setjmp/longjmp implementation for kernel->user->kernel context switching.
; Field order Must match struct kcontext in kernel/cpu/kcontext.h
; rbx=0, rbp=8, r12=16, r13=24, r14=32, r15=40, rsp=48, rip=56, rflags=64
; SysV ABI: 1st arg = rdi (struct kcontext *), 2nd arg = rsi (return value)

global kernel_ctx_save
global kernel_ctx_restore
global kernel_ctx_switch
; void kernel_ctx_switch(struct kcontext *save_to, struct kcontext *restore_from) [save_to in rdi, restore_from in rsi]
; save_to  in RDI - the OUTGOING process: save its context to this struct
; restore_from in RSI - the INCOMING process: restore its context from this struct
kernel_ctx_switch:
    ; --- save outgoing context RDI (save_to) ---
    mov [rdi + 0],  rbx
    mov [rdi + 8],  rbp
    mov [rdi + 16], r12
    mov [rdi + 24], r13
    mov [rdi + 32], r14
    mov [rdi + 40], r15

    lea rax, [rsp + 8]           ; caller's RSP (skip our return address)
    mov [rdi + 48], rax
    mov rax, [rsp]               ; resume RIP = our return address
    mov [rdi + 56], rax

    pushfq                       ; save RFLAGS to stack
    pop rax                      ; pop RFLAGS into rax
    ; Force IF=1 in the saved snapshot. kernel_ctx_switch is called from
    ; inside interrupt-gate handlers (the LAPIC timer ISR, or a syscall via
    ; int 0x80) which auto-clear IF on entry -- so the LIVE flags at this
    ; exact instruction read IF=0 regardless of what the outgoing process
    ; was actually running with. But reaching this point at all (a hardware
    ; IRQ was serviced, or a software int 0x80 fired) proves the outgoing
    ; process had IF=1 the moment it was interrupted/trapped: a maskable
    ; IRQ literally cannot be taken while IF=0, and ring 3 can't execute
    ; cli/sti at all (privileged, #GP). Storing the raw mid-ISR flags here
    ; instead would make this process resume with interrupts permanently
    ; off after its first preemption -- its next hlt would never wake.
    or rax, 0x200
    mov [rdi + 64], rax          ; save RFLAGS

    ; --- restore incoming context RSI (restore_from) ---
    mov rbx, [rsi + 0]
    mov rbp, [rsi + 8]
    mov r12, [rsi + 16]
    mov r13, [rsi + 24]
    mov r14, [rsi + 32]
    mov r15, [rsi + 40]
    mov rsp, [rsi + 48]          ; switch back to the saved kernel stack

    mov rax, [rsi + 64]          ; restore RFLAGS
    push rax                      ; push RFLAGS onto stack
    popfq                        ; restore RFLAGS from stack

    mov rcx, [rsi + 56]          ; resume right after kernel_ctx_save
    jmp rcx                      ; jump to the saved RIP

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
    pushfq                       ; save RFLAGS to stack
    pop rax                      ; pop RFLAGS into rax
    or rax, 0x200                ; force IF=1 on resume -- see kernel_ctx_switch's
                                  ; comment; kept consistent even though today's
                                  ; only caller (usermode.c) isn't inside an ISR.
    mov [rdi + 64], rax          ; save RFLAGS

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

    mov rax, [rdi + 64]          ; restore RFLAGS
    push rax                      ; push RFLAGS onto stack
    popfq                        ; restore RFLAGS from stack
    
    jmp rcx                      ; jump to the saved RIP