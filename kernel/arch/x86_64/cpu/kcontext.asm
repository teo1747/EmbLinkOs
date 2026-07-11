; Minimal setjmp/longjmp implementation for kernel->user->kernel context switching.
; Field order Must match struct kcontext in kernel/cpu/kcontext.h
; rbx=0, rbp=8, r12=16, r13=24, r14=32, r15=40, rsp=48, rip=56, rflags=64
; SysV ABI: 1st arg = rdi (struct kcontext *), 2nd arg = rsi (return value)

global kernel_ctx_save
global kernel_ctx_restore
global kernel_ctx_switch
; void kernel_ctx_switch(struct kcontext *save_to, struct kcontext *restore_from,
;                         void *fpu_save_to, void *fpu_restore_from)
; [save_to=rdi, restore_from=rsi, fpu_save_to=rdx, fpu_restore_from=rcx]
; save_to  in RDI - the OUTGOING process: save its context to this struct
; restore_from in RSI - the INCOMING process: restore its context from this struct
; fpu_save_to/fpu_restore_from: 512-byte, 16-byte-aligned FXSAVE/FXRSTOR
; images (struct thread::fpu_state, process.h) for the same outgoing/incoming
; pair. Requires fpu_init_this_cpu() to have already run on this core.
kernel_ctx_switch:
    ; Outgoing FPU/SSE state first -- rdx is untouched by every instruction
    ; below until the function returns/jumps, so there's no ordering
    ; constraint here beyond "before we ever leave". FXSAVE reads only its
    ; memory operand's address (rdx); it doesn't clobber any GP register.
    fxsave [rdx]

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
    ; Do NOT force IF=1 here (kernel_ctx_save below still does, for a
    ; different caller -- see its own comment). kernel_ctx_switch has
    ; exactly ONE call site in the whole kernel: schedule_locked()
    ; (process.c), immediately followed by spin_unlock(&g_sched_lock).
    ; That means EVERY future resumption of this saved context -- via
    ; ANY core, ANY tick -- lands at that exact spin_unlock call, which
    ; ITSELF decides whether to re-enable interrupts, based on
    ; g_sched_lock's own saved_flags (captured by whichever core is
    ; dispatching this resume, reflecting THAT core's real pre-lock
    ; state). Forcing IF=1 here made interrupts go live via this popfq
    ; below, one `jmp` instruction before that spin_unlock call actually
    ; runs -- a real, narrow but genuinely hit window where this core's
    ; own next timer tick could fire in between, re-enter schedule() and
    ; spin_lock(&g_sched_lock) while THIS core still holds it (the jmp
    ; hadn't executed yet, so we're still logically inside the same
    ; schedule_locked() call), self-deadlocking. Leaving IF as it
    ; genuinely was (0, since every save happens from inside
    ; lapic_timer_handler's ISR) closes that window: interrupts only
    ; go live inside spin_unlock's own cli-protected sequence, never via
    ; a jmp that could be interrupted first.
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

    ; Incoming FPU/SSE state. rcx still holds our own 4th argument
    ; (fpu_restore_from) here -- nothing above this line touches it, so this
    ; MUST happen before the next instruction overwrites rcx with the resume
    ; RIP.
    fxrstor [rcx]

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