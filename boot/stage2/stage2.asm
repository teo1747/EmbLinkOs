[BITS 16]
[ORG 0x7E00]

%define KERNEL_LOAD_SECTORS 1024

start:
    
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti

    mov si, msg_stage2
    call print_string

    call vbe_init

    call e820_query
    call enable_a20

    ; Load kernel one sector at a time, starting at LBA 9.
    mov word  [dap + 2], 1          ; 1 sector per read
    mov word  [dap + 4], 0x0000     ; offset
    mov word  [dap + 6], 0x1000     ; segment (= phys 0x10000)
    mov dword [dap + 8], 9          ; initial LBA
    mov dword [dap + 12], 0

    mov cx, KERNEL_LOAD_SECTORS

.read_loop:
    mov si, dap
    mov ah, 0x42
    mov dl, 0x80
    int 0x13
    jc disk_error_S2


    add word  [dap + 6], 0x20
    add dword [dap + 8], 1
    adc dword [dap + 12], 0

    loop .read_loop

    xor ax, ax
    mov es, ax

    cli
    lgdt [gdt_descriptor]

    mov eax, cr0
    or eax, 0x1
    mov cr0, eax

    jmp 0x08:protected_mode

;Disk address packet for LBA read
dap:
    db 0x10                  ; Packet size (16 bytes)
    db 0                     ; Reserved
    dw 1                   ; number of sectors per INT 13h read (patched above too)
    dw 0x0000                ; Starting head
    dw 0x1000                ; Starting segment (loads to 0x10000)
    dq 9                     ; Starting LBA (sector 10, 0-indexed = LDA 9)


no_lba_support:
    mov si, msg_no_lba
    call print_string
    jmp $

lba_done:


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

e820_query:
    ; This function will query the memory map using BIOS interrupt 0x15, function 0xE820

    pusha                    
    
    mov di, 0x7004            ; Buffer to store the memory map entry 
    xor ebx, ebx              ; Set EBX to 0 for the first call
    xor bp, bp                ; Clear BP to use as a counter for the number of entries
    mov edx, 0x534D4150       ; 'SMAP' signature
    mov eax, 0xE820           ; E820 function number
    mov [es:di + 20], dword 1 ; Force a valid ACPI 3.X entry
    mov ecx, 24               ; Size of the buffer for each entry (20 bytes for the entry + 4 bytes for the ACPI 3.X flag)
    int 0x15                  ; Call BIOS interrupt
    jc .failed                ; If carry flag is set, there are no more entries / don't support E820

    cmp eax, 0x534D4150       ; Check if the returned signature is correct
    jne .failed

    test ebx, ebx              ; Check if EBX is zero, which indicates the last entry / or none since it the first call
    je .failed
    jmp .got_entry

.next_entry:
    mov eax, 0xE820           ; 
    mov [es:di + 20], dword 1 
    mov ecx, 24  
    mov edx, 0x534D4150             
    int 0x15                  ; Call BIOS interrupt
    jc .done                  ; If carry flag is set, there are no more entries 

    mov eax, 0x534D4150

.got_entry:
    jcxz .skip_entry ; If EBX is zero, skip processing this entry since it indicates the last entry \
    cmp cl, 20              ; Check if the returned entry size is at least 20 bytes (the size of the standard E820 entry)
    jbe .accept
    test dword [es:di + 20], 1 ; Check if the ACPI 3.X flag is set, which indicates that the entry is valid even if it's larger than 20 bytes
    je .skip_entry       ; If the ACPI 3.X flag is not set, skip this entry since it's not valid

.accept:
    ; At this point, we have a valid memory map entry in the buffer at es:
    mov ecx, [es:di + 8]    ; check lehgth low
    or ecx, [es:di + 12]    ; check length high
    jz .skip_entry       ; If the length is zero, skip this entry since it doesn't describe any usable memory

    inc bp                  ; Increment the entry counter
    add di, 24              ; Move to the next entry (size of the buffer for each entry)

.skip_entry:
    test ebx, ebx          ; Check if EBX is zero, which indicates the last entry / or none since it the first call
    jne .next_entry 


.done:
    mov [0x7000], bp        ; Store the number of valid entries in a known location for later use
    clc                     ; Clear carry flag to indicate success
    popa       
    ret

.failed:
    mov [0x7000], 0         
    stc                     ; Set carry flag to indicate failure              
    popa
    ret


vbe_init:
    ; This function will initialize the VBE (Video BIOS Extension) and return the address of the VBE control structure
    ; INT 10h AX=4FOOH - get VBE controller info
    
    mov ax, 0x4F00           ; Get VBE controller information
    mov di, 0x5000           ; Address of VBE info buffer
    int 0x10                 ; Call BIOS video interrupt
    cmp ax, 0x004F           ; Check if the function returned successfully
    jne .vbe_failed 
 
    ; This function will get the mode information for the VBE mode specified by the parameter
    ; INT 10h AX=4F01 - get VBE mode info
                     
    mov ax, 0x4F01           ; Get VBE mode information
    mov cx, 0x118            ; Mode
    mov di, 0x5200        ; Address of VBE mode info buffer
    int 0x10                 ; Call BIOS video interrupt
    cmp ax, 0x004F           ; Check if the function returned successfully
    jne .vbe_failed    


    ; Set up the VBE mode
    ; INT 10h AX=4F02 - set VBE mode

    mov ax, 0x4F02           ; Set VBE mode
    mov bx, 0x118 | 0x4000  ; Mode 
    mov esi, 0x6000          ; Address of VBE mode info buffer
    int 0x10                 ; Call BIOS video interrupt
    cmp ax, 0x004F           ; Check if the function returned successfully
    jne .vbe_failed 



    ; Step 4: Copy field to 0x6000
    ; ModeInfoBlock    ofsets:
    ; 0x00 = Mode Attributes (16 bits)
    ; 0x10 = PBytesPerScanLine (16 bits pitch)
    ; 0x12 = XResolution (16 bits width)
    ; 0x14 = YResolution (16 bits height
    ; 0x19 = BitsPerPixel (8 bits depth)
    ; 0x28 = PhysBasePtr (32 bits framebuffer address)

    mov esi, 0x5200          ; Address of VBE mode info buffer

    xor eax, eax
    mov eax, [esi + 0x28]  
    mov [0x6000], eax       ; lower 32 bits of PhysBasePtr
    mov dword [0x6004], 0   ; higher 32 bits of PhysBasePtr (set to 0)

    ;width
    xor eax, eax
    mov ax, [esi + 0x12]
    mov [0x6008], eax

    ;height
    xor eax, eax
    mov ax, [esi + 0x14]
    mov [0x600C], eax

    ;pitch
    xor eax, eax
    mov ax, [esi + 0x10]
    mov [0x6010], eax

    ;depth bpp
    xor eax, eax
    mov al, [esi + 0x19]
    mov [0x6014], eax

   
    ; Format - read from VBE mode info buffer
    xor eax, eax
    mov al, [esi + 0x20] ; read field position 0x20
    cmp al, 0
    je .rgb
    mov dword [0x6018], 1 ; 32-bit 

    ret

.rgb:
    mov dword [0x6018], 0 

    ret


.vbe_failed:
   
    mov si, msg_vbe_failed
    call print_string
    jmp $

    

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

    ;===Higher-half mapping===
    ;PD[511] -> p4_table_higher (this is 0XFFFFFFFF80000000) PML4
    mov eax, p3_table_higher
    or eax, 0x03            ; Present and Read/Write
    mov [p4_table + 511*8], eax; Set P[511] to point to the PDPT_higher

    ;PDPT_higher[510] -> p2_table (same PD as identity map - shares the mapping)
    mov eax, p2_table
    or eax, 0x03            ; Present and Read/Write
    mov [p3_table_higher + 510 * 8], eax 

    ;=== PD: map 512 * 2MB = 1GB using huge pages ===
    mov ecx, 0
.map_p2:
    mov eax, 0x200000 ; 2MB page
    mul ecx              ; page index
    or eax, 0b10000011  ; Present, Read/Write, User/Supervisor, Page Size = 2MB
    mov [p2_table + ecx * 8], eax ; Set the page table entry for this page
    inc ecx              ; Increment the page index
    cmp ecx, 512         ; Check if we have mapped all 512 pages
    jl .map_p2          ; If not, map the next page
    ret                  ; Return from the function



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
    mov ecx, 4 * 4096 / 4   ; 4 tables × 4KB / 4 bytes each
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

    ; Use higher half vitual address space for stack
    mov rsp, 0xFFFFFFFF80200000         ; Set stack pointer to 0x200000  

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


msg_stage2    db 'Helios Stage 2 loading...', 0x0D, 0x0A, 0 ; Message to display (null-terminated)

msg_protected db 'Welcome to Helios Protected Mode!', 0 ; Message to display in protected mode (null-terminated)

msg_longmode  db 'Welcome to Helios Long Mode!', 0 ; Message to display in long mode (null-terminated)

msg_error_S2   db 'Kernel loading failed!', 0x0D, 0x0A, 0 ; Error message for stage 2 (null-terminated)

msg_error_efl  db 'ELF loading failed!', 0 ; Error message for ELF loading (null-terminated)

msg_no_lba db 'BIOS does not support LBA!', 0x0D, 0x0A, 0

msg_vbe_failed db 'VBE initialization failed!', 0x0D, 0x0A, 0 ; Error message for VBE initialization (null-terminated)


; Page tables at fixed addresses (must be 4KB aligned)
p4_table equ 0x9000          ; PML4
p3_table equ 0xA000          ; PDPT for lower half
p2_table equ 0xB000          ; PD shared by both maps
p3_table_higher equ 0xc000   ; PDPT for higher half



; Pad to exactly 4096 bytes so kernel starts at sector 3
times 4096 - ($ - $$) db 0