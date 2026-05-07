[BITS 16]
[ORG 0x7E00]


    mov si, msg_stage2       ; Load the address of the stage 2 message into SI
    call print_string         ; Call the print_string function

    ; step 1: enable A20 line
    call enable_a20

    cli                     ; Clear interrupts before switching to protected mode

    ; step 2: load GDT
    lgdt [gdt_descriptor]     ; Load the GDT descriptor into GDTR

    ; step 3: switch to protected mode
    mov eax, cr0            
    or eax, 0x1              ; Set the PE bit (Protection Enable)
    mov cr0, eax             

    ; step 4: far jump to flush the instruction pipeline and switch to protected mode
    jmp 0x08:protected_mode ; Jump to the protected mode code segment


enable_a20:
    mov ax, 0x2401          ; Prepare to enable A20 line
    int 0x15                ; Call BIOS interrupt to enable A20
    ret                     

gdt_start:

gdt_null:
    dq 0                     ; Null descriptor (8 bytes)

gdt_code:
    dw 0xFFFF                ; Limit (low 16 bits)
    dw 0x0000                ; Base (low 16 bits)
    db 0x00                  ; Base (middle 8 bits)
    db 10011010b             ; Type and attributes (code segment)
    db 11001111b             ; Limit (high 4 bits) and Base (high 8 bits)
    db 0x00                  ; Base (high 8 bits)

gdt_data:
    dw 0xFFFF                ; Limit (low 16 bits)
    dw 0x0000                ; Base (low 16 bits)
    db 0x00                  ; Base (middle 8 bits)
    db 10010010b             ; Type and attributes (data segment)
    db 11001111b             ; Limit (high 4 bits) and Base (high 8 bits)
    db 0x00                  ; Base (high 8 bits)

gdt_end:


gdt_descriptor:
    dw gdt_end - gdt_start - 1 ; Limit
    dd gdt_start             ; Base



[BITS 32]
protected_mode:
    ; At this point, we are in protected mode and can use 32-bit instructions
    
    
    mov ax, 0x10            ; Load the data segment selector (index 2 in GDT)
    mov ds, ax              ; Set DS to the data segment
    mov es, ax              ; Set ES to the data segment
    mov fs, ax              ; Set FS to the data segment
    mov gs, ax              ; Set GS to the data segment
    mov ss, ax              ; Set SS to the data segment
    mov esp, 0x9000         ; Set stack pointer to 0x9000

    mov esi, msg_protected   
    call print_string_pm
    jmp $                       

print_string_pm:
    pusha                    
    mov edx, 0xB8000          ; VGA text mode buffer
.loop:
    mov al, [esi]             ; Load byte at DS:ESI into AL
    or al, al               ; Check if AL is zero (end of string)
    jz .done
    mov ah, 0x0F            ; Attribute byte (white on black)
    mov [edx], ax           ; Write character and attribute to VGA buffer
    add edx, 2              ; Move to the next character cell (2 bytes)
    inc esi                 ; Move to the next character in the string
    jmp .loop
.done:
    popa                     
    ret

[BITS 16]
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

msg_stage2    db 'MyOS Stage 2 loading...', 0x0D, 0x0A, 0 ; Message to display (null-terminated)

msg_protected db 'Welcome to MyOS Protected Mode!', 0 ; Message to display in protected mode (null-terminated)