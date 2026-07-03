; Kernel entry trampoline.
;
; stage2 loads the kernel ELF and jumps to its e_entry with a *temporary* stack
; that lives in low conventional memory (set up by stage2). Our very first job is
; to switch RSP onto the kernel's OWN stack, which lives in .bss and is therefore
; part of the kernel image: it is mapped by the kernel mapping and reserved by the
; PMM automatically, and it grows *with* the kernel instead of sitting at a fixed
; low-memory address the growing kernel can crash into. Then we call kernel_main.

[BITS 64]

section .bss
align 4096
boot_stack_bottom:
    resb 0x20000                 ; 128 KiB kernel boot stack
global boot_stack_top
boot_stack_top:                  ; full-descending stack starts here (page-aligned)

section .text
global _start
extern kernel_main
_start:
    mov rsp, boot_stack_top      ; switch to the kernel-owned stack
    xor rbp, rbp                 ; terminate stack-trace/frame chain
    call kernel_main
.halt:                           ; kernel_main should never return; park the CPU
    cli
    hlt
    jmp .halt
