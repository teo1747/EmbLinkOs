/* the minimum ELF64 needed to load a static executable */
#include <stdint.h>

struct elf64_ehdr {
    uint8_t  e_ident[16];   /* [0..3] = 0x7F 'E' 'L' 'F'; [4]=class; [5]=data */
    uint16_t e_type;        /* 2 = ET_EXEC (static executable)               */
    uint16_t e_machine;     /* 0x3E = x86-64                                 */
    uint32_t e_version;
    uint64_t e_entry;       /* >>> jump here <<<                             */
    uint64_t e_phoff;       /* file offset of the program-header array       */
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;   /* size of ONE program header                    */
    uint16_t e_phnum;       /* how many program headers                      */
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed));

struct elf64_phdr {
    uint32_t p_type;        /* 1 = PT_LOAD (the ones we load)                */
    uint32_t p_flags;       /* PF_X=1, PF_W=2, PF_R=4                        */
    uint64_t p_offset;      /* byte offset in the file                       */
    uint64_t p_vaddr;       /* virtual address to load at                    */
    uint64_t p_paddr;       /* physical addr — ignored for our purposes      */
    uint64_t p_filesz;      /* bytes present in the file                     */
    uint64_t p_memsz;       /* bytes in memory (>= filesz; extra is .bss)    */
    uint64_t p_align;
} __attribute__((packed));

#define PT_LOAD   1
#define PF_X      1
#define PF_W      2
#define PF_R      4
#define ET_EXEC   2
#define EM_X86_64 0x3E

int elf_load(const uint8_t *image, uint64_t image_len, uint64_t pml4_phys, uint64_t *entry_out);

int elf_load_from_file(const char *path, uint64_t pml4_phys, uint64_t *entry_out);