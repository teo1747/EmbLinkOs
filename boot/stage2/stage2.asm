[BITS 16]
[ORG 0x7E00]

start:
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



gdt64_start:

gdt64_null:
    dq 0                     ; Null descriptor (8 bytes)

gdt64_code:
    dw 0x0000                ; Limit (low 16 bits)
    dw 0x0000                ; Base (low 16 bits)
    db 0x00                  ; Base (middle 8 bits) 
    db 10011010b             ; Type and attributes (code segment)(p = 1, dpl = 00, s = 1, e = 1, rw = 1)
    db 00100000b             ; Limit (high 4 bits) and Base (high 8 bits) (long mode code segment has a limit of 0)
    db 0x00                  ; Base (high 8 bits)

gdt64_data:
    dw 0x0000                ; Limit (low 16 bits)
    dw 0x0000                ; Base (low 16 bits)
    db 0x00                  ; Base (middle 8 bits) 
    db 10010010b             ; Type and attributes (data segment)(p = 1, dpl = 00, s = 1, e = 0, rw = 1)
    db 00000000b             ; Limit (high 4 bits) and Base (high 8 bits) (long mode data segment has a limit of 0)
    db 0x00                  ; Base (high 8 bits)

gdt64_end:

gdt64_descriptor:
    dw gdt64_end - gdt64_start - 1 ; Limit
    dd gdt64_start           ; Base


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

    call zero_page_tables     ; Clear the page tables before using them
    call setup_page_tables     ; Set up page tables for protected mode (identity mapping for the first 1GB)
    call enable_paging          ; Enable paging and long mode

    lgdt [gdt64_descriptor]     ; Load the 64-bit GDT descriptor into GDTR
    jmp 0x08:long_mode_start          

    mov esi, msg_protected   
    call print_string_pm
    jmp $    

setup_page_tables:
    ; This is where you would set up your page tables for paging
    ; For simplicity, we will identity map the first 1GB of memory
    ; You would need to fill in the page tables and then enable paging by setting the PG bit in CR0  

    ;PML4[0] -> p3_table
    mov eax, p3_table
    or eax, 0x03            ; Present and Read/Write
    mov [p4_table], eax     ; Set PML4[0] to point to the PDPT

    ;PDPT[0] -> p2_table
    mov eax, p2_table
    or eax, 0x03            ; Present and Read/Write
    mov [p3_table], eax     ; Set PDPT[0] to point to the PD

    ;PD[0] -> 2MB page (maps 512 x 2MB = 1GB)
    mov ecx, 0
.map_p2:
    mov eax, 0x200000 ; 2MB 
    mul ecx              ; eax = 2MB * ecx  calculates the physical address for the page
    or eax, 0b10000011        ; Present, Read/Write, huge (ps bit) 
    mov [p2_table + ecx*8], eax ; Set PD[ecx] to point to the 2MB page
    inc ecx
    cmp ecx, 512            ; We need to map 512 entries to cover 1GB
    jl .map_p2
    ret

    ; After setting up the page tables, you would enable paging by setting the PG bit in CR0

enable_paging:
    ; Load the address of the PML4 table into CR3 to enable paging
    mov eax, p4_table 
    mov cr3, eax              

    ; Enable PAE by setting the PAE bit in CR4
    mov eax, cr4
    or eax, (1 << 5)              ; Set the PAE bit
    mov cr4, eax

    ; Enable long mode by setting the LME bit in EFER MSR
    ; To access MSRs, we need to use the RDMSR and WRMSR instructions. The EFER MSR is at index 0xC0000080.
    mov ecx, 0xC0000080          ; EFER MSR comvention to use ecx to specify the MSR index
    rdmsr
    or eax, (1 << 8)              ; Set the LME bit
    wrmsr

    ; Enable paging by setting the PG bit in CR0
    mov eax, cr0
    or eax, (1 << 31)             ; Set the PG bit (Paging Enable)
    mov cr0, eax

    ret

zero_page_tables:
    mov edi, 0x9000         ; start of p4_table
    mov ecx, 3 * 4096 / 4  ; 3 tables × 4KB / 4 bytes each
    xor eax, eax
    rep stosd               ; fill with zeros
    ret

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






[BITS 64]

long_mode_start:
    ; This is where you would continue with your long mode code
    ; You can set up your kernel, initialize devices, etc.

    mov ax, 0x10            ; Load the data segment selector (index 2 in GDT)
    mov ds, ax              
    mov es, ax              
    mov fs, ax              
    mov gs, ax              
    mov ss, ax              


    mov rsi, msg_longmode     
    call print_string_64
    jmp $               

print_string_64:
    push rax
    push rdx
    push rsi
    mov rdx, 0xB8000          ; VGA text mode buffer
.loop:
    mov al, [rsi]             ; Load byte at DS:RSI into AL
    or al, al               
    jz .done
    mov ah, 0x0F            ; Attribute byte (white on black)
    mov [rdx], ax           
    add rdx, 2              
    inc rsi                 
    jmp .loop               
.done:
    pop rsi
    pop rdx
    pop rax
    ret
    








msg_stage2    db 'MyOS Stage 2 loading...', 0x0D, 0x0A, 0 ; Message to display (null-terminated)

msg_protected db 'Welcome to MyOS Protected Mode!', 0 ; Message to display in protected mode (null-terminated)

msg_longmode  db 'Welcome to MyOS Long Mode!', 0 ; Message to display in long mode (null-terminated)

; Page tables at fixed addresses (must be 4KB aligned)
p4_table equ 0x9000     ; PML4
p3_table equ 0xA000     ; PDPT
p2_table equ 0xB000     ; PD