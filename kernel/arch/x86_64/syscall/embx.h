#ifndef __EMBX_H__
#define __EMBX_H__

#include <stdint.h>
#include "include/types.h"

/* EMBX (EmbLink Binary eXecutable) -- the native executable format. This header
 * is the container from EMBX_Specification_v2 §3, byte-exact, and the loader in
 * embx.c is §6 written down. Only the APP contract (§4.1) has a load path in
 * v1; every other binary_type is refused (§6 step 6). The one thing this format
 * carries that ELF cannot is the capability table (§5) -- the reason it exists.
 */

#define EMBX_MAGIC0 0x7F
/* "EMBX\r\n\x1A" -- the CRLF pair breaks LOUDLY on any text-mode mangling, the
 * \x1A stops a naive cat from spraying the image at a terminal (§3.1). */
static const uint8_t EMBX_MAGIC[8] = { 0x7F,'E','M','B','X',0x0D,0x0A,0x1A };

#define EMBX_TYPE_APP    1
#define EMBX_MACHINE_X64 1
#define EMBX_ABI_VERSION 1          /* the ABI this kernel provides */

#define EMBX_SEG_NULL 0
#define EMBX_SEG_LOAD 1

#define EMBX_SEG_R 1
#define EMBX_SEG_W 2
#define EMBX_SEG_X 4

/* v1 understands no incompat feature (§3.6): any set bit means refuse. */
#define EMBX_KNOWN_INCOMPAT 0ULL

struct embx_header {
    uint8_t  magic[8];                 /* 0x00 */
    uint16_t version_major;            /* 0x08 */
    uint16_t version_minor;            /* 0x0A */
    uint32_t header_size;              /* 0x0C  == 128 in v1 */
    uint16_t binary_type;              /* 0x10 */
    uint16_t machine;                  /* 0x12 */
    uint32_t abi_version;              /* 0x14 */
    uint64_t flags;                    /* 0x18 */
    uint64_t feature_incompat;         /* 0x20 */
    uint64_t feature_compat;           /* 0x28 */
    uint64_t entry_point;              /* 0x30 */
    uint32_t segment_table_offset;     /* 0x38 */
    uint16_t segment_count;            /* 0x3C */
    uint16_t segment_entry_size;       /* 0x3E  == 64 */
    uint32_t capability_table_offset;  /* 0x40 */
    uint16_t capability_count;         /* 0x44 */
    uint16_t capability_entry_size;    /* 0x46  == 16 */
    uint32_t grantor;                  /* 0x48  reserved, MUST be 0 in v1 */
    uint32_t reserved0;                /* 0x4C */
    uint8_t  build_id[32];             /* 0x50  SHA-256 */
    uint64_t image_size;               /* 0x70 */
    uint32_t reserved1;                /* 0x78 */
    uint32_t header_checksum;          /* 0x7C  CRC32C over 0x00..0x7B */
} __attribute__((packed));
_Static_assert(sizeof(struct embx_header) == 128, "EMBX header must be 128 bytes");

/* CRC32C span, derived from the struct so it can never drift (§3.1). */
#define EMBX_HDR_BODY_SIZE (sizeof(struct embx_header) - sizeof(uint32_t))

struct embx_segment {
    uint32_t type;          /* 0x00 */
    uint32_t flags;         /* 0x04  R/W/X, meaningful for LOAD */
    uint64_t vaddr;         /* 0x08 */
    uint64_t file_offset;   /* 0x10 */
    uint64_t file_size;     /* 0x18 */
    uint64_t mem_size;      /* 0x20  >= file_size (the tail is BSS) */
    uint64_t align;         /* 0x28  power of two, >= 4096 for LOAD */
    uint32_t checksum;      /* 0x30  CRC32C over file_size bytes */
    uint32_t reserved0;     /* 0x34 */
    uint64_t paddr;         /* 0x38  BOOT-only; MUST be 0 elsewhere */
} __attribute__((packed));
_Static_assert(sizeof(struct embx_segment) == 64, "EMBX segment entry must be 64 bytes");

struct embx_capability {
    uint32_t cap_id;        /* 0x00 */
    uint32_t cap_flags;     /* 0x04  reserved, MUST be 0 in v1 */
    uint64_t reserved0;     /* 0x08  reserved for a v2 parameter, MUST be 0 */
} __attribute__((packed));
_Static_assert(sizeof(struct embx_capability) == 16, "EMBX capability entry must be 16 bytes");

/* True if the first bytes are an EMBX magic -- used to dispatch a spawn between
 * the ELF loader and this one. */
bool embx_is_magic(const uint8_t *first8);

/* Load an EMBX APP into `pml4_phys` (§6, steps 1-11). `grantor_caps` is the
 * spawning process's capability set: the binary's DECLARED capabilities must be
 * a subset of it (step 9) or the load is refused. On success writes the entry
 * point and the granted (== declared) capability set, which the caller installs
 * as the new process's cap_set. Nothing is mapped until every validation
 * (steps 1-9) has passed. Returns -EMBK_* (EPERM specifically for a capability
 * denial, so it is distinguishable). */
int embx_load_from_file(const char *path, uint64_t pml4_phys,
                        uint64_t grantor_caps,
                        uint64_t *entry_out, uint64_t *granted_caps_out);

#endif /* __EMBX_H__ */
