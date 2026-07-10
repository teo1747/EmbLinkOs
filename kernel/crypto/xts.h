#ifndef _EMBK_CRYPTO_XTS_H
#define _EMBK_CRYPTO_XTS_H

#include <stdint.h>
#include <stddef.h>
#include "aes.h"

/*
 * AES-256-XTS (IEEE P1619 / NIST SP 800-38E), built on aes.h.
 *
 * Design decisions for EMBKFS v2 (Phase 4), recorded here since they're
 * deliberate simplifications relative to "full" XTS, not oversights:
 *
 *   - The tweak (XTS calls it the "data unit number") is always the
 *     extent's on-disk block number, encoded as a little-endian uint64_t
 *     in the low 8 bytes of the 16-byte tweak input, high 8 bytes zero.
 *     This means encryption metadata needs zero extra on-disk bytes beyond
 *     the EXTENT_F_ENCRYPTED flag bit -- the block number IS the nonce,
 *     and it's already on disk as the extent's own address.
 *
 *   - Ciphertext stealing (XTS's mechanism for handling a final partial
 *     16-byte block) is NOT implemented. EMBKFS extents are always whole,
 *     block-size-rounded runs, so every call site always passes a length
 *     that's a multiple of 16 bytes -- ciphertext stealing would only ever
 *     matter for slack space we never actually encrypt as a unit anyway.
 *
 *   - One "sector" (data unit) = one on-disk filesystem block. The alpha
 *     multiplication advances the tweak once per 16-byte AES block WITHIN
 *     that filesystem block; crossing a block boundary always starts a
 *     fresh tweak derived from the new block's own number, not a
 *     continuation of the previous block's tweak sequence.
 */

struct aes_xts_ctx {
    struct aes256_ctx data;
    struct aes256_ctx tweak;
};

void aes_xts_init(struct aes_xts_ctx *ctx,
                   const uint8_t data_key[AES256_KEY_SIZE],
                   const uint8_t tweak_key[AES256_KEY_SIZE]);

/* len must be a multiple of AES_BLOCK_SIZE (16). in/out may alias. */
void aes_xts_encrypt(const struct aes_xts_ctx *ctx, uint64_t block_number,
                      const uint8_t *in, uint8_t *out, size_t len);
void aes_xts_decrypt(const struct aes_xts_ctx *ctx, uint64_t block_number,
                      const uint8_t *in, uint8_t *out, size_t len);

/* Round-trip + differential tests (changing the key, block number, or
 * plaintext must change the ciphertext), plus a 4-AES-block known-answer
 * vector cross-checked against Python's `cryptography` package
 * (modes.XTS) using this exact tweak encoding -- an external oracle, not
 * just internal self-consistency. */
int aes_xts_run_selftests(void);

#endif /* _EMBK_CRYPTO_XTS_H */
