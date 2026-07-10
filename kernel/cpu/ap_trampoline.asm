; Real-mode entry point for application processors (APs), started via
; INIT-SIPI-SIPI (kernel/cpu/lapic.c's lapic_start_ap()). Assembled as a
; standalone flat binary (like boot/stage1, boot/stage2 -- see the Makefile)
; and copied to AP_TRAMPOLINE_PHYS (kernel/cpu/smp.h) by the BSP before
; each SIPI. NOT linked into the main kernel image: an AP starts in 16-bit
; real mode and must physically execute below 1MB, which the main kernel's
; higher-half-linked ELF is not.
;
; Three data slots near the end (ap_pml4_phys, ap_stack_top, ap_entry_point)
; are poked by the BSP (kernel/cpu/smp.c's smp_bringup()) with per-AP
; values before each SIPI: which PML4 to load (always the kernel's own --
; see kernel/cpu/percpu.h's comment on why this needs a permanent identity
; map of low memory, added by ap_bootstrap_map() in smp.c), which stack
; this specific AP should use, and the address of ap_entry64
; (kernel/cpu/ap_entry.asm) to jump to once paging and long mode are live.
;
; This mirrors boot/stage2/stage2.asm's own real->protected->long mode
; transition (same GDT shapes, same CR0/CR4/EFER sequence), simplified: an
; AP reuses the kernel's ALREADY-BUILT page tables instead of constructing
; its own, so there is no page-table-setup step here at all.

BITS 16
org 0x8000
DEFAULT ABS   ; absolute (not RIP-relative) addressing throughout -- this
              ; blob is copied verbatim to a fixed physical address
              ; (AP_TRAMPOLINE_PHYS) and never relocated, so every label
              ; reference should resolve to its real, fixed linear address

ap_trampoline_start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x8FF0          ; scratch stack within this same reserved page
                            ; (see pmm_reserve_page(AP_TRAMPOLINE_PHYS) in
                            ; pmm.c) -- only used for this brief real/
                            ; protected-mode window, discarded once
                            ; long_mode_ap below loads the real per-AP stack

    lgdt [gdt32_descriptor]

    mov eax, cr0
    or eax, 1               ; CR0.PE
    mov cr0, eax

    jmp 0x08:protected_mode_ap

BITS 32
protected_mode_ap:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x8FF0

    ; Enable PAE (required before EFER.LME can be set)
    mov eax, cr4
    or eax, (1 << 5)
    mov cr4, eax

    ; Load CR3 with the kernel's OWN PML4 (poked by the BSP below) -- low
    ; 32 bits only, exactly like stage2.asm's own CR3 load: this kernel's
    ; PML4 is always allocated well below 4GB (early PMM allocation), so
    ; the upper 32 bits are correctly zero.
    mov eax, [ap_pml4_phys]
    mov cr3, eax

    ; Enable long mode (EFER.LME)
    mov ecx, 0xC0000080
    rdmsr
    or eax, (1 << 8)
    wrmsr

    ; Enable paging (CR0.PG) -- the instant this executes, paging is live
    ; using the kernel's real page tables. The NEXT instruction fetch is
    ; still at this low physical address, which is exactly why
    ; ap_bootstrap_map() (smp.c) permanently identity-maps [0, 1MB) into
    ; the kernel's own PML4 before any AP is ever started -- without that,
    ; this would immediately page-fault.
    mov eax, cr0
    or eax, (1 << 31)
    mov cr0, eax

    lgdt [gdt64_descriptor]
    jmp 0x08:long_mode_ap

BITS 64
long_mode_ap:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    mov rsp, [ap_stack_top]    ; this AP's own kernel stack -- a normal
                                ; higher-half kernel-virtual address,
                                ; already valid: CR3 above points at the
                                ; SAME page tables the BSP itself uses
    mov rax, [ap_entry_point]  ; ap_entry64's real, linked kernel-virtual
                                ; address (kernel/cpu/ap_entry.asm)
    jmp rax


; --- Temporary GDTs, used only for this real->protected->long transition.
; Deliberately separate from the kernel's own real GDT (cpu/gdt.c) rather
; than reused: keeps this delicate low-level bring-up code fully
; self-contained. ap_entry64 calls gdt_init_this_cpu() almost immediately,
; which loads the REAL per-core GDT/TSS and makes these descriptors
; irrelevant from that point on -- so they never need to describe anything
; beyond "flat, present, correct type," matching stage2.asm's own minimal
; transitional GDTs exactly.

align 8
gdt32_start:
    dq 0                        ; null descriptor
gdt32_code:
    dw 0xFFFF, 0x0000
    db 0x00, 10011010b, 11001111b, 0x00
gdt32_data:
    dw 0xFFFF, 0x0000
    db 0x00, 10010010b, 11001111b, 0x00
gdt32_end:

gdt32_descriptor:
    dw gdt32_end - gdt32_start - 1
    dd gdt32_start

align 8
gdt64_start:
    dq 0                        ; null descriptor
gdt64_code:
    dw 0x0000, 0x0000
    db 0x00, 10011010b, 00100000b, 0x00
gdt64_data:
    dw 0x0000, 0x0000
    db 0x00, 10010010b, 00000000b, 0x00
gdt64_end:

gdt64_descriptor:
    dw gdt64_end - gdt64_start - 1
    dd gdt64_start


; --- Data slots poked by the BSP before each SIPI (kernel/cpu/smp.c) ---
align 8
ap_pml4_phys:
    dq 0
ap_stack_top:
    dq 0
ap_entry_point:
    dq 0

ap_trampoline_end:
