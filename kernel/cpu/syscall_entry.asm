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

; kernel_ctx_save / kernel_ctx_restore now live in kcontext.asm.




















