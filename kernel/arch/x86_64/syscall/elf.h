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

#define PT_LOAD    1
#define PT_DYNAMIC 2
#define PF_X      1
#define PF_W      2
#define PF_R      4
#define ET_EXEC   2
#define ET_DYN    3
#define EM_X86_64 0x3E

/* --- dynamic linking (Phase 2): the app is ET_EXEC + DT_NEEDED libembk.so, the
 * toolkit lives in a PIC ET_DYN .so; the kernel is the loader (no ld.so). --- */
struct elf64_dyn { int64_t d_tag; uint64_t d_val; } __attribute__((packed));
struct elf64_sym {
    uint32_t st_name;  uint8_t st_info; uint8_t st_other;
    uint16_t st_shndx; uint64_t st_value; uint64_t st_size;
} __attribute__((packed));
struct elf64_rela { uint64_t r_offset; uint64_t r_info; int64_t r_addend; } __attribute__((packed));

/* dynamic tags */
#define DT_NULL 0
#define DT_NEEDED 1
#define DT_PLTRELSZ 2
#define DT_PLTGOT 3
#define DT_HASH 4
#define DT_STRTAB 5
#define DT_SYMTAB 6
#define DT_RELA 7
#define DT_RELASZ 8
#define DT_RELAENT 9
#define DT_STRSZ 10
#define DT_SYMENT 11
#define DT_PLTREL 20
#define DT_JMPREL 23

/* x86-64 relocation types we handle */
#define R_X86_64_64        1
#define R_X86_64_COPY      5
#define R_X86_64_GLOB_DAT  6
#define R_X86_64_JUMP_SLOT 7
#define R_X86_64_RELATIVE  8

#define SHN_UNDEF 0
#define SHN_ABS   0xfff1        /* st_value is an absolute value, NOT a VA */

/* st_info packs bind in the high nibble. STB_WEAK matters to the loader: an
 * UNDEFINED weak symbol is not an error -- it binds to 0 by definition, and
 * code that references one is required to test it before use. */
#define ELF64_ST_BIND(i) ((uint8_t)((i) >> 4))
#define STB_GLOBAL 1
#define STB_WEAK   2

#define ELF64_R_SYM(i)  ((uint32_t)((i) >> 32))
#define ELF64_R_TYPE(i) ((uint32_t)((i) & 0xffffffffULL))

/* Load bias for the (single) shared library -- clear of the app (low), the
 * shared-window VA (0x5000...), the heap (0x6000...) and the stack (0x7000...). */
#define DYLIB_VA_BASE 0x0000200000000000ULL

int elf_load(const uint8_t *image, uint64_t image_len, uint64_t pml4_phys, uint64_t *entry_out);

int elf_load_from_file(const char *path, uint64_t pml4_phys, uint64_t *entry_out);