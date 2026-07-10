; Embeds the assembled AP trampoline (kernel/cpu/ap_trampoline.asm, built
; separately as a raw flat binary -- see the Makefile) into the main kernel
; image as plain data, so C code (kernel/cpu/smp.c's smp_bringup()) can
; find and copy it into low memory at runtime without the kernel needing
; its own filesystem to be up yet (it isn't, this early in boot).
;
; Same technique this tree previously used for embedding user/init.elf
; before that switched to loading it from disk (see the now-removed
; kernel/cpu/init_blob.asm, referenced in the Makefile's history) --
; `incbin` pulls in the raw bytes, the two labels bracket them so C can
; compute the length at runtime instead of hardcoding it.

section .rodata

global ap_trampoline_blob_start
global ap_trampoline_blob_end

ap_trampoline_blob_start:
    incbin "kernel/cpu/ap_trampoline.bin"
ap_trampoline_blob_end:
