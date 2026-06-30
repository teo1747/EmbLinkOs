extern syscall_dispatch
global syscall_entry

; Entered via `int 0x80` (legacy) from ring 3 or `syscall` (modern later). CPU has already pushed the return RIP, CS, and RFLAGS onto the stack.
; and switched to RSP0 the (the TSS kernel stack). We push the GPRs to complete a
; struct regs, hand it's pointer to the syscall handler, and then pop the GPRs and return to userland.
syscall_entry:
    push rax                  ; push the syscall number (in rax) onto the stack
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11        
    push r12
    push r13
    push r14
    push r15


    mov rdi, rsp              ; pass pointer to struct regs (on stack) in rdi
    call syscall_dispatch     ; call the syscall handler

    pop r15
    pop r14
    pop r13 
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8  
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax                  ; pop the syscall number (in rax) off the stack

    iret                      ; return to userland (RIP, CS, RFLAGS popped by CPU)


; --- minimal setjmp/longjmp: return from a ring-3 task without a scheduler ----
; struct kcontext { rbx, rbp, r12, r13, r14, r15, rsp, rip }  (offsets 0..56).
; enter_user_mode saves a context before iretq'ing to ring 3; sys_exit restores
; it (instead of halting) to unwind back into enter_user_mode and resume the
; kernel. Only callee-saved regs + RSP + the resume RIP need saving.

global kernel_ctx_save
kernel_ctx_save:                 ; rdi = struct kcontext *
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
    xor eax, eax                 ; the direct call returns 0
    ret

global kernel_ctx_restore
kernel_ctx_restore:              ; rdi = struct kcontext *, rsi = return value
    mov rbx, [rdi + 0]
    mov rbp, [rdi + 8]
    mov r12, [rdi + 16]
    mov r13, [rdi + 24]
    mov r14, [rdi + 32]
    mov r15, [rdi + 40]
    mov rax, rsi                 ; value kernel_ctx_save appears to return
    mov rsp, [rdi + 48]          ; switch back to the saved kernel stack
    jmp [rdi + 56]               ; resume right after kernel_ctx_save




















