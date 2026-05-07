[BITS 16]

[ORG 0x7C00]    
; Set up the stack



start:
    cli                     ; Clear interrupts
    xor ax, ax              ; Zero out AX
    mov ds, ax              ; Set DS to 0
    mov es, ax              ; Set ES to 0
    mov ss, ax              ; Set SS to 0
    mov sp, 0x7C00          ; Set stack pointer to the beginning of the bootloader
    sti                     ; Enable interrupts

    mov si, msg_hello       ; Load the address of the message into SI
    call print_string       ; Call the print_string function
    
    ; load stage 2 from disk 
    mov ah, 0x02            ; BIOS read sector function
    mov al, 1               ; read 1 sector
    mov ch, 0               ; Cylinder 0
    mov cl, 2               ; Sector 2 (first sector is 1)
    mov dh, 0               ; Head 0
    mov dl, 0x80            ; Drive 0 (first hard disk)
    mov bx, 0x7E00          ; Load the sector into memory at 0x7E00
    int 0x13                ; Call BIOS disk interrupt
    JC disk_error           ; If carry flag is set, there was an error

    jmp 0x7E00              ; Jump to the loaded stage 2 code

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


disk_error:
    mov si, msg_error   ; Load the address of the error message into SI
    call print_string   ; Call the print_string function
    jmp $               ; Infinite loop to halt the system


msg_hello db 'MyOS Stage 1 loading...', 0x0D, 0x0A, 0 ; Message to display (null-terminated)
msg_error db 'Disk read error!', 0x0D, 0x0A, 0 ; Error message (null-terminated)


; Bootloader signature (must be 0x55AA at the end of the 512-byte sector)
times 510 - ($ - $$) db 0 ; Pad the rest of the sector with zeros
dw 0xAA55                  ; Boot signature