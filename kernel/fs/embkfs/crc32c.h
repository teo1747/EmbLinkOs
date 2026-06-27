#ifndef _EMBKFS_CRC32C_H
#define _EMBKFS_CRC32C_H

#include <stdint.h>
#include <stddef.h>

/*
 * CRC32C (Castagnoli) — the single checksum primitive for all of EMBKFS:
 * the superblock, every tree node (the Merkle checksums), every block
 * pointer, every extent's data checksum, and the directory-entry name hash.
 *
 * This is a byte-for-byte software port of the host formatter's crc32c.py
 * (the oracle). It is intentionally self-contained — only <stdint.h> and
 * <stddef.h>, no kernel headers — so it produces the identical value
 * everywhere and could compile unchanged in userspace. A CPUID-gated SSE4.2
 * `crc32` fast path can be added later behind this same signature (TODO.md).
 *
 * Parameters, which MUST match the oracle exactly:
 *   polynomial 0x82F63B78 (Castagnoli, reflected), init 0xFFFFFFFF,
 *   input + output reflected, final XOR 0xFFFFFFFF.
 * Canonical check value: crc32c("123456789") == 0xE3069283.
 */

/* Build the 256-entry lookup table. Call ONCE at boot before any
 * embk_crc32c() call — embkfs_init() does this. */
void embk_crc32c_init(void);

/* CRC32C over `len` bytes at `data`. Pass seed = 0 for a fresh checksum;
 * pass a previous return value to continue a running CRC over concatenated
 * buffers. */
uint32_t embk_crc32c(const void *data, size_t len, uint32_t seed);

#endif /* _EMBKFS_CRC32C_H */