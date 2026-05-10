[BITS 16]
[ORG 0x7E00]

start:
    mov si, msg_stage2       ; Load the address of the stage 2 message into SI
    call print_string         ; Call the print_string function

    ; step 1: enable A20 line
    call enable_a20

    ;mov bx, 0x10000        ; Load the sector into memory at 0x10000 (16-bit mode can only address up to 1MB, so we use segment:offset to access memory above 64KB)
    mov ax, 0x1000          ; Load the sector into memory at 0x10000 (segment:offset = 0x1000:0x0000)
        mov es, ax          ; Set ES to 0x1000 so that the kernel is loaded at physical address 0x10000
        xor bx, bx          ; offset 0x0000(es:bx = 0x1000:0x0000 = 0x10000)

    ;load kernel from disk 
    mov ah, 0x02            ; BIOS read sector function
    mov al, 30           
    mov ch, 0               ; Cylinder 0
    mov cl, 10              ; Sector 10 (first sector is 1)              
    mov dh, 0               
    mov dl, 0x80            ; Drive 0 (first hard disk)
   
    int 0x13                ; Call BIOS disk interrupt
    JC disk_error_S2        ; If carry flag is set, there was an error 

    xor ax, ax              ; Clear AX to set ES back to 0 for future use
    mov es, ax              ; restore ES to 0 for future use

    cli                     ; Clear interrupts before switching to protected mode

    ; step 2: load GDT
    lgdt [gdt_descriptor]     ; Load the GDT descriptor into GDTR
     

    ; step 3: switch to protected mode
    mov eax, cr0            
    or eax, 0x1              ; Set the PE bit (Protection Enable)
    mov cr0, eax             

    ; step 4: far jump to flush the instruction pipeline and switch to protected mode
    jmp 0x08:protected_mode ; Jump to the protected mode code segment

disk_error_S2:
    mov si, msg_error_S2   
    call print_string      
    jmp $                  


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
    mov rsp, 0x200000         ; Set stack pointer to 0x200000  

    mov rsi, msg_longmode     
    call print_string_64

    ;load the kernel from memory (it should have been loaded by stage 2 at 0x10000)
    call load_elf

    ;jmp to the kernel entry point 
    jmp rax ;

load_elf:
    ; This is where you would parse the ELF header, load the program segments into memory,
    ; and then return the entry point address in RAX

    mov rsi, 0x10000         ; Address where the ELF file is loaded

    ; verify ELF magic number
    cmp dword [rsi], 0x464C457F ; "\x7FELF" in little-endian
    jne elf_err

    ; save the ELF header fields we need for loading the segments
    mov rax, [rsi + 0x18] ; e_entry (entry point) - 0x18 is the offset of e_entry in the ELF header
    push rax                ; Save the entry point address on the stack to return to later


    ; read program header info
    mov r8, [rsi + 0x20] ; e_phoff (program header offset) - 0x20 is the offset of e_phoff in the ELF header
    movzx r9, word [rsi + 0x38] ; e_phnum (number of program headers) - 0x38 is the offset of e_phnum in the ELF header
    movzx r10, word [rsi + 0x36] ; e_phentsize (size of each program header) - 0x36 is the offset of e_phentsize in the ELF header

    ; rsi points to the start of the ELF file, we need to add e_phoff to get to the program headers
    add r8, rsi             ; r8 now points to the first program header

.next_segment:
    ; Check if we've processed all program headers
    test r9, r9           ; Check if e_phnum is zero
    je .done_loading

    ; check if this is a loadable segment (p_type == (1)PT_LOAD)
    cmp dword [r8], 1        ; p_type is the first field in the program header
    jne .skip_segment 

    push r8                ; Save the pointer to the current program header on the stack for use in loading the segment
    push rsi                ; Save rsi since we'll need it to point to the ELF file for loading the segment

    ; Load the segment into memory
    mov rdi, [r8 + 0x10] ; p_vaddr (virtual address to load the segment) - 0x10 is the offset of p_vaddr in the program header
    mov rcx, [r8 + 0x20] ; p_filesz (size of the segment in the file) - 0x20 is the offset of p_filesz in the program header
    mov rbx, [r8 + 0x08] ; p_offset (offset of the segment in the file) - 0x08 is the offset of p_offset in the program header
    add rsi, rbx         ; rsi now points to the start of the segment data in the ELF file

    ; Copy the segment data to its virtual address in memory
    rep movsb            ; Copy rcx bytes from [rsi] to [rdi]

    pop rsi                
    pop r8              
        ; If p_memsz > p_filesz, we need to zero out the remaining bytes in memory
    
    ; zero out the rest of the segment if p_memsz > p_filesz()
    mov rdi, [r8 + 0x10] ; 
    add rdi, [r8 + 0x20] ; rdi now points to the end of the loaded segment in memory
    mov rcx, [r8 + 0x28] ; p_memsz (size of the segment in memory) - 0x28 is the offset of p_memsz in the program header
    sub rcx, [r8 + 0x20] ; Calculate the number of bytes to zero out (p_memsz - p_filesz)
    xor eax, eax         ; Zero out RAX to use for filling with zeros
    rep stosb            ; Fill the remaining bytes with zeros(rcx bytes from eax to [rdi])

.skip_segment:
    add r8, r10           ; Move to the next program header (e_phentsize)
    dec r9                ; Decrement the program header count
    jmp .next_segment

.done_loading:
    ; After loading all segments, we can get the entry point address from the ELF header
    pop rax                ; Get the entry point address from the ELF header (offset 0x18)
    ret


elf_err:
    ; Handle ELF loading error (e.g., print an error message and halt)
    mov rsi, msg_error_efl   
    call print_string_64      
    jmp $                   ; Infinite loop to halt the system 
                  

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

msg_error_S2   db 'Kernel loading failed!', 0x0D, 0x0A, 0 ; Error message for stage 2 (null-terminated)

msg_error_efl  db 'ELF loading failed!', 0 ; Error message for ELF loading (null-terminated)

; Page tables at fixed addresses (must be 4KB aligned)
p4_table equ 0x9000     ; PML4
p3_table equ 0xA000     ; PDPT
p2_table equ 0xB000     ; PD



; Pad to exactly 4096 bytes so kernel starts at sector 3
times 4096 - ($ - $$) db 0