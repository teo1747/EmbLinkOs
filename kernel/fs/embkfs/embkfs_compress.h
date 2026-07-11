#ifndef _EMBKFS_COMPRESS_H
#define _EMBKFS_COMPRESS_H

#include <stdint.h>
#include "include/types.h"

/*
 * A small, self-contained LZ77-family compressor for EMBKFS v2's per-extent
 * compression (Phase 3). LZ4-INSPIRED, not a byte-exact LZ4 encoder/decoder:
 * the token layout (literal-length nibble + match-length nibble + 255-byte
 * length extension, exactly like real LZ4) is the same idea, but this
 * format has no end-of-block marker -- embk_decompress() is always told the
 * exact expected output length up front (EMBKFS already stores that as the
 * extent's logical_size), so the decoder simply stops once it has produced
 * that many bytes rather than needing a specific "final sequence" encoding.
 * That's a deliberate simplification, not an attempt at LZ4 interop.
 */

/* Worst-case output size for compressing src_len bytes (all-literal
 * encoding plus token overhead) -- size a scratch/destination buffer with
 * this, not with src_len itself. */
uint32_t embk_compress_bound(uint32_t src_len);

/* Compresses src[0..src_len) into dst (capacity dst_cap bytes). Returns
 * true and sets *out_len only if the compressed form both fits in dst_cap
 * AND is smaller than src_len -- callers should keep data uncompressed
 * whenever this returns false, rather than treat it as an error. */
bool embk_compress(const uint8_t *src, uint32_t src_len,
                    uint8_t *dst, uint32_t dst_cap, uint32_t *out_len);

/* Decompresses src[0..src_len) into dst, producing EXACTLY expected_len
 * bytes. Returns false if the stream is malformed or would over/underflow
 * expected_len (a corrupt compressed extent should fail loudly, not hand
 * back a truncated or overrun buffer). */
bool embk_decompress(const uint8_t *src, uint32_t src_len,
                      uint8_t *dst, uint32_t expected_len);

/* Round-trip tests across all-zero, repeating, and pseudo-random data
 * (compressible and incompressible cases), plus a check that highly
 * compressible input actually shrinks and incompressible input is
 * correctly reported as not worth keeping compressed. */
int embk_compress_run_selftests(void);

#endif /* _EMBKFS_COMPRESS_H */
