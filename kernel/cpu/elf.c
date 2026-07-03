/* kernel/cpu/elf.c  (or fold into usermode.c) */
#include "elf.h"
#include "../mm/vmm.h"
#include "../mm/pmm.h"
#include "../drivers/serial.h"
#include "../include/errno.h"

#define PAGE_SIZE_4K 0x1000ULL
#define PAGE_DOWN(x) ((x) & ~(PAGE_SIZE_4K - 1))
#define PAGE_UP(x)   (((x) + PAGE_SIZE_4K - 1) & ~(PAGE_SIZE_4K - 1))

/* Translate ELF p_flags into VMM flags. NOTE the inversion: executable is the
 * ABSENCE of NX; non-exec data SETS NX. Write maps VMM_WRITABLE. Always USER. */
static uint64_t elf_flags_to_vmm(uint32_t p_flags)
{
    uint64_t f = VMM_USER;
    if (p_flags & PF_W) f |= VMM_WRITABLE;
    if (!(p_flags & PF_X)) f |= VMM_NX;     /* no exec permission -> set NX */
    return f;
}

/* Load a static ELF64 executable already resident in memory at `image`
 * (image_len bytes). On success writes the entry point to *entry_out. */
int elf_load(const uint8_t *image, uint64_t image_len, uint64_t pml4_phys, uint64_t *entry_out)
{   
    if (!image || !entry_out)
        return -EMBK_EINVAL;
    if (!pml4_phys)
        return -EMBK_EINVAL;


    if (image_len < sizeof(struct elf64_ehdr))
        return -EMBK_EINVAL;
    
    const struct elf64_ehdr *eh = (const struct elf64_ehdr *)image;

    /* Validate: magic, 64-bit, x86-64, static executable. Fail loudly. */
    if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L'  || eh->e_ident[3] != 'F')
        return -EMBK_EINVAL;                 /* not an ELF */
    if (eh->e_ident[4] != 2)                 /* ELFCLASS64 */
        return -EMBK_EINVAL;
    if (eh->e_type != ET_EXEC || eh->e_machine != EM_X86_64)
        return -EMBK_EINVAL;
    if (eh->e_phoff + (uint64_t)eh->e_phnum * eh->e_phentsize > image_len)
        return -EMBK_EINVAL;                 /* phdr array out of bounds */

    const struct elf64_phdr *ph =
        (const struct elf64_phdr *)(image + eh->e_phoff);

    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const struct elf64_phdr *p = &ph[i];
        if (p->p_type != PT_LOAD)
            continue;                        /* only PT_LOAD gets loaded */
        if (p->p_filesz > p->p_memsz)
            return -EMBK_EINVAL;             /* malformed */
        if (p->p_offset + p->p_filesz > image_len)
            return -EMBK_EINVAL;             /* file bytes out of bounds */
        if (p->p_vaddr >= DIRECT_MAP_BASE)   /* <-- restore this */
            return -EMBK_EINVAL;             /* user must load lower-half only */


        uint64_t seg_start = PAGE_DOWN(p->p_vaddr);
        uint64_t seg_end   = PAGE_UP(p->p_vaddr + p->p_memsz);

        /* Map the segment's pages WRITABLE for now, so the copy below can land,
         * even if the segment is meant to be read-only. We tighten afterward. */
        for (uint64_t va = seg_start; va < seg_end; va += PAGE_SIZE_4K) {
            uint64_t phys = pmm_alloc_page();
            if (!phys)
                return -EMBK_ENOMEM;
            vmm_map_in(pml4_phys, va, phys, VMM_USER | VMM_WRITABLE);
        }

        /* Copy p_filesz bytes from the file image to the segment, then zero the
         * .bss tail up to p_memsz. The target pages live ONLY in pml4_phys, which
         * is NOT the current address space (we haven't switched CR3 yet), so we
         * can't write through p_vaddr. Instead reach each frame we just mapped via
         * the kernel direct map (P2V) and copy into it there. */
        const uint8_t *src = image + p->p_offset;
        for (uint64_t va = seg_start; va < seg_end; va += PAGE_SIZE_4K) {
            uint64_t phys = vmm_get_phys_in(pml4_phys, va) & ~0xFFFULL;
            uint8_t *page = (uint8_t *)P2V(phys);

            for (uint64_t off = 0; off < PAGE_SIZE_4K; off++) {
                uint64_t v = va + off;          /* virtual addr of this byte */
                if (v >= p->p_vaddr && v < p->p_vaddr + p->p_filesz)
                    page[off] = src[v - p->p_vaddr];   /* file-backed byte */
                else
                    page[off] = 0;              /* alignment slack / .bss tail */
            }
        }

        /* Now tighten permissions to the real p_flags (drops WRITABLE on code,
         * sets NX on data). Re-map each page with the correct frame + flags. */
        uint64_t vmm_flags = elf_flags_to_vmm(p->p_flags);
        for (uint64_t va = seg_start; va < seg_end; va += PAGE_SIZE_4K) {
            uint64_t phys = vmm_get_phys_in(pml4_phys, va) & ~0xFFFULL;  /* the frame we mapped */
            vmm_map_in(pml4_phys, va, phys, vmm_flags);   /* re-flag, same frame */
        }

        serial_write_string("ELF: loaded segment vaddr=");
        serial_write_hex(p->p_vaddr);
        serial_write_string(" filesz=");
        serial_write_hex(p->p_filesz);
        serial_write_string(" memsz=");
        serial_write_hex(p->p_memsz);
        serial_write_string(p->p_flags & PF_X ? " [R-X]\n" : " [RW-]\n");
    }

    *entry_out = eh->e_entry;
    return EMBK_OK;
}