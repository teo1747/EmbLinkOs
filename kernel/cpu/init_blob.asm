
; Embeds the flat user binary into the kernel image and brackets it with two
; labels so C can recover both the bytes AND the length at runtime.
section .rodata
global init_blob_start
global init_blob_end

init_blob_start:
    incbin "user/init.bin"      ; the raw bytes land right here
init_blob_end:                  ; label immediately after -> end address