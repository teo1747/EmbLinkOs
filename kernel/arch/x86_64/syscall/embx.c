/* kernel/arch/x86_64/syscall/embx.c -- the EMBX APP loader.
 *
 * This file IS EMBX_Specification_v2 §6: embx_load_from_file walks the numbered
 * load sequence, and every rejection is a parse-time refusal (§8) BEFORE a
 * single byte is mapped. Steps 1-9 validate a candidate image (including the
 * capability check); step 10 is the first irreversible act. A rejected binary
 * costs one page table's worth of nothing.
 *
 * Mirrors elf.c's segment mapping (pmm_alloc_page + vmm_map_in, copy through
 * the direct map, W^X via the NX-is-absence-of-X inversion) -- the machinery is
 * the same, only the container it reads from differs. */
#include "arch/x86_64/syscall/embx.h"
#include "fs/vfs.h"
#include "fs/fd.h"
#include "include/kmalloc.h"
#include "mm/vmm.h"
#include "mm/pmm.h"
#include "drivers/char/serial.h"
#include "include/errno.h"
#include "fs/embkfs/crc32c.h"        /* embk_crc32c -- the house checksum */
#include "process/capabilities.h"
#include "arch/x86_64/syscall/usercopy.h"   /* USER_VA_LIMIT */

#define PAGE_SIZE_4K 0x1000ULL
#define PAGE_DOWN(x) ((x) & ~(PAGE_SIZE_4K - 1))
#define PAGE_UP(x)   (((x) + PAGE_SIZE_4K - 1) & ~(PAGE_SIZE_4K - 1))

/* Only the syscall/usercopy.c copy sees USER_VA_LIMIT via its own header; make
 * sure it is visible here for the §8 low-half guard even if that header path
 * changes. */
#ifndef USER_VA_LIMIT
#define USER_VA_LIMIT 0x0000800000000000ULL
#endif

/* A rejection: log the reason (elf.c's style) and return the code. The reason
 * string is the whole point of distinct guards -- "denied X" beats "bad file". */
static int embx_reject(const char *dev_unused, const char *why, int code) {
    (void)dev_unused;
    serial_write_string("EMBX load: ");
    serial_write_string(why);
    serial_write_string("\n");
    return code;
}

/* Read the whole file into a fresh kmalloc'd buffer (caller frees). Same shape
 * as elf.c's read_file_kbuf; kept local so the two loaders don't couple. */
static int embx_read_file(const char *path, uint8_t **out_buf, uint64_t *out_len) {
    int fd = vfs_open(path, O_RDONLY, 0);
    if (fd < 0) return fd;
    struct vfs_stat st;
    int rc = vfs_fd_fstat(fd, &st);
    if (rc != EMBK_OK) { vfs_close(fd); return rc; }
    if (st.size < sizeof(struct embx_header)) { vfs_close(fd); return -EMBK_EINVAL; }
    uint8_t *buf = kmalloc(st.size);
    if (!buf) { vfs_close(fd); return -EMBK_ENOMEM; }
    uint64_t total = 0;
    while (total < st.size) {
        size_t got = 0;
        rc = vfs_fd_read(fd, buf + total, (size_t)(st.size - total), &got);
        if (rc != EMBK_OK || got == 0) { kfree(buf); vfs_close(fd); return -EMBK_EIO; }
        total += got;
    }
    vfs_close(fd);
    *out_buf = buf; *out_len = total;
    return EMBK_OK;
}

bool embx_is_magic(const uint8_t *first8) {
    for (int i = 0; i < 8; i++) if (first8[i] != EMBX_MAGIC[i]) return false;
    return true;
}

static bool is_pow2_ge_4k(uint64_t a) {
    return a >= PAGE_SIZE_4K && (a & (a - 1)) == 0;
}

/* Translate EMBX segment R/W/X into VMM flags. Same inversion as elf.c:
 * executable is the ABSENCE of NX. Always USER. */
static uint64_t seg_flags_to_vmm(uint32_t f) {
    uint64_t v = VMM_USER;
    if (f & EMBX_SEG_W) v |= VMM_WRITABLE;
    if (!(f & EMBX_SEG_X)) v |= VMM_NX;
    return v;
}

int embx_load_from_file(const char *path, uint64_t pml4_phys,
                        uint64_t grantor_caps,
                        uint64_t *entry_out, uint64_t *granted_caps_out) {
    uint8_t *img = NULL;
    uint64_t len = 0;
    int rc = embx_read_file(path, &img, &len);
    if (rc != EMBK_OK) return rc;

    #define FAIL(why, code) do { kfree(img); return embx_reject(0, (why), (code)); } while (0)

    /* --- steps 1-9: pure validation, nothing mapped --- */

    /* (1) enough for a header */
    if (len < sizeof(struct embx_header)) FAIL("truncated: shorter than a header", -EMBK_ENOEXEC);
    const struct embx_header *h = (const struct embx_header *)img;

    /* (2) magic */
    if (!embx_is_magic(h->magic)) FAIL("bad magic", -EMBK_ENOEXEC);

    /* (3) header checksum BEFORE any offset inside the header is trusted */
    if (embk_crc32c(img, EMBX_HDR_BODY_SIZE, 0) != h->header_checksum)
        FAIL("header checksum mismatch", -EMBK_ENOEXEC);

    /* (4) version + header size */
    if (h->version_major > 1 || h->header_size != sizeof(struct embx_header))
        FAIL("unsupported version / header size", -EMBK_ENOEXEC);

    /* (5) unknown incompat feature */
    if (h->feature_incompat & ~EMBX_KNOWN_INCOMPAT)
        FAIL("unknown incompat feature bit", -EMBK_ENOEXEC);

    /* (6) type -- only APP has a load path in v1 */
    if (h->binary_type != EMBX_TYPE_APP) FAIL("not an APP (this loader loads only APP)", -EMBK_ENOEXEC);

    /* (7) machine + abi */
    if (h->machine != EMBX_MACHINE_X64) FAIL("wrong machine", -EMBK_ENOEXEC);
    if (h->abi_version > EMBX_ABI_VERSION) FAIL("binary needs a newer ABI than this kernel", -EMBK_ENOEXEC);

    /* (8) size + table sanity + entry-size stride */
    if (h->image_size != len) FAIL("image_size != file length", -EMBK_ENOEXEC);
    if (h->grantor != 0 || h->reserved0 != 0 || h->reserved1 != 0)
        FAIL("reserved/grantor field nonzero", -EMBK_ENOEXEC);
    if (h->segment_entry_size != sizeof(struct embx_segment) ||
        h->capability_entry_size != sizeof(struct embx_capability))
        FAIL("bad table entry stride", -EMBK_ENOEXEC);

    uint64_t seg_off = h->segment_table_offset;
    uint64_t seg_end = seg_off + (uint64_t)h->segment_count * sizeof(struct embx_segment);
    if (seg_off < sizeof(struct embx_header) || seg_end > len)
        FAIL("segment table out of range", -EMBK_ENOEXEC);

    uint64_t cap_off = h->capability_table_offset;
    if (h->capability_count > 0) {
        uint64_t cap_end = cap_off + (uint64_t)h->capability_count * sizeof(struct embx_capability);
        if (cap_off < sizeof(struct embx_header) || cap_end > len)
            FAIL("capability table out of range", -EMBK_ENOEXEC);
    } else if (cap_off != 0) {
        FAIL("capability_count 0 but table offset nonzero", -EMBK_ENOEXEC);
    }

    const struct embx_segment *segs = (const struct embx_segment *)(img + seg_off);

    /* validate every segment (§8) before mapping any */
    uint64_t prev_vend = 0;
    bool have_prev = false;
    bool entry_in_x = false;
    for (uint16_t i = 0; i < h->segment_count; i++) {
        const struct embx_segment *s = &segs[i];
        if (s->paddr != 0) FAIL("paddr nonzero on a non-BOOT kind", -EMBK_ENOEXEC);
        if (s->type != EMBX_SEG_LOAD) continue;    /* non-LOAD types are not mapped */

        if (s->mem_size < s->file_size) FAIL("mem_size < file_size", -EMBK_ENOEXEC);
        if (!is_pow2_ge_4k(s->align)) FAIL("align not a power of two >= 4096", -EMBK_ENOEXEC);
        if ((s->flags & EMBX_SEG_W) && (s->flags & EMBX_SEG_X)) FAIL("W and X both set (W^X)", -EMBK_ENOEXEC);
        if ((s->vaddr % s->align) != (s->file_offset % s->align)) FAIL("vaddr/file_offset not congruent mod align", -EMBK_ENOEXEC);
        if (s->file_offset + s->file_size > len) FAIL("segment payload past end of file", -EMBK_ENOEXEC);
        if (s->vaddr + s->mem_size >= USER_VA_LIMIT) FAIL("segment reaches the kernel half", -EMBK_ENOEXEC);
        if (have_prev && s->vaddr < prev_vend) FAIL("LOAD segments overlap or are not sorted by vaddr", -EMBK_ENOEXEC);
        prev_vend = s->vaddr + s->mem_size; have_prev = true;

        if (h->entry_point >= s->vaddr && h->entry_point < s->vaddr + s->mem_size
            && (s->flags & EMBX_SEG_X))
            entry_in_x = true;
    }
    if (h->entry_point == 0 || !entry_in_x)
        FAIL("entry_point not inside an executable LOAD segment", -EMBK_ENOEXEC);

    /* (9) THE CAPABILITY CHECK. sorted + unique + known, then subset of grantor.
     * A denial is -EMBK_EPERM specifically, so the caller can distinguish
     * "declared a capability it was not granted" from "malformed image". */
    uint64_t declared = 0;
    const struct embx_capability *caps = (const struct embx_capability *)(img + cap_off);
    uint32_t prev_id = 0;
    for (uint16_t i = 0; i < h->capability_count; i++) {
        const struct embx_capability *c = &caps[i];
        if (c->cap_flags != 0 || c->reserved0 != 0) FAIL("capability reserved field nonzero", -EMBK_ENOEXEC);
        if (c->cap_id == 0 || c->cap_id > EMBK_CAP_MAX_ID) FAIL("capability id 0 or unknown to this kernel", -EMBK_ENOEXEC);
        if (i > 0 && c->cap_id <= prev_id) FAIL("capability table not sorted-ascending / has a duplicate", -EMBK_ENOEXEC);
        prev_id = c->cap_id;
        declared |= EMBK_CAP_BIT(c->cap_id);
    }
    uint64_t granted;
    if (embk_caps_attenuate(grantor_caps, declared, &granted) != 0) {
        /* name the offending class -- the shell wants "denied: declared X". */
        uint64_t missing = declared & ~grantor_caps;
        serial_write_string("EMBX load: DENIED -- binary declares capabilities the "
                            "grantor does not hold (missing mask 0x");
        char hex[17]; const char *d = "0123456789abcdef"; int p = 0;
        for (int sh = 60; sh >= 0; sh -= 4) { uint8_t nib = (missing >> sh) & 0xF; if (p || nib || sh == 0) hex[p++] = d[nib]; }
        hex[p] = 0; serial_write_string(hex); serial_write_string(")\n");
        kfree(img);
        return -EMBK_EPERM;               /* EMBX_ECAPS */
    }

    /* --- step 10: the first irreversible act. Map each LOAD segment. --- */
    for (uint16_t i = 0; i < h->segment_count; i++) {
        const struct embx_segment *s = &segs[i];
        if (s->type != EMBX_SEG_LOAD) continue;

        /* verify the payload's checksum before trusting its bytes */
        if (s->file_size &&
            embk_crc32c(img + s->file_offset, (uint32_t)s->file_size, 0) != s->checksum)
            FAIL("segment checksum mismatch", -EMBK_ENOEXEC);

        uint64_t vstart = s->vaddr;
        uint64_t seg_start = PAGE_DOWN(vstart);
        uint64_t seg_final = PAGE_UP(vstart + s->mem_size);
        uint64_t vmm_flags = seg_flags_to_vmm(s->flags);

        for (uint64_t va = seg_start; va < seg_final; va += PAGE_SIZE_4K) {
            uint64_t phys = pmm_alloc_page();
            if (!phys) FAIL("out of memory mapping a segment", -EMBK_ENOMEM);
            /* writable first so the copy lands, tighten to real flags after */
            vmm_map_in(pml4_phys, va, phys, VMM_USER | VMM_WRITABLE);
            uint8_t *page = (uint8_t *)P2V(phys);
            for (uint64_t off = 0; off < PAGE_SIZE_4K; off++) {
                uint64_t v = va + off;
                if (v >= vstart && v < vstart + s->file_size)
                    page[off] = img[s->file_offset + (v - vstart)];
                else
                    page[off] = 0;         /* the .bss tail + alignment slack */
            }
            vmm_map_in(pml4_phys, va, phys, vmm_flags);
        }
    }

    /* step 11: report the entry point and the granted set */
    if (entry_out) *entry_out = h->entry_point;
    if (granted_caps_out) *granted_caps_out = granted;

    serial_write_string("EMBX load: OK\n");
    kfree(img);
    #undef FAIL
    return EMBK_OK;
}
