[BITS 16]
[ORG 0x7E00]


    mov si, msg_stage2       ; Load the address of the stage 2 message into SI
    call print_string         ; Call the print_string function
    jmp $                     ; Infinite loop to halt the system

print_string:
    pusha                    ; Save all registers
.loop:
    lodsb                   ; Load byte at DS:SI into AL and increment SI
    or al, al               ; Check if AL is zero (end of string)
    jz .done                ; If zero, jump to done
    mov ah, 0x0E            ; BIOS teletype function
    mov bh, 0x00            ; Page number
    int 0x10                ; Call BIOS video interrupt
    jmp .loop               ; Repeat for the next character
.done:
    popa                     ; Restore all registers
    ret                      ; Return from the function

msg_stage2 db 'MyOS Stage 2 loading...', 0x0D, 0x0A, 0 ; Message to display (null-terminated)
