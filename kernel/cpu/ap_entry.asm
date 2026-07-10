; 64-bit entry point an AP jumps to from kernel/cpu/ap_trampoline.asm, once
; that trampoline has enabled paging (using the kernel's own, already-built
; page tables) and long mode. Unlike kentry.asm's _start (which still has
; to switch off a temporary bootloader stack), RSP is ALREADY set correctly
; here -- the trampoline poked it with this AP's own per-core stack top
; (kernel/cpu/percpu.h's struct cpu_data::rsp0_stack) before jumping here.
; This is linked into the main kernel image (unlike the trampoline itself),
; so its address is a normal higher-half kernel-virtual address -- exactly
; the value kernel/cpu/smp.c's smp_bringup() pokes into the trampoline's
; ap_entry_point data slot before each SIPI.

[BITS 64]

section .text
global ap_entry64
extern ap_main
ap_entry64:
    xor rbp, rbp          ; terminate stack-trace/frame chain, same as kentry.asm's _start
    call ap_main
.halt:                     ; ap_main should never return; park this core
    cli
    hlt
    jmp .halt
