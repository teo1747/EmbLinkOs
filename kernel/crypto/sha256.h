#ifndef _EMBK_CRYPTO_SHA256_H
#define _EMBK_CRYPTO_SHA256_H

#include <stdint.h>
#include <stddef.h>

/*
 * SHA-256 (FIPS 180-4), built from scratch for EMBKFS v2 -- the foundation
 * HMAC-SHA256 (hmac.h) and PBKDF2-HMAC-SHA256 (pbkdf2.h) are both built on.
 * Self-contained: only <stdint.h>/<stddef.h>, no kernel headers, same
 * portability discipline as crc32c.h.
 */

#define SHA256_BLOCK_SIZE  64
#define SHA256_DIGEST_SIZE 32

struct sha256_ctx {
    uint32_t state[8];
    uint64_t bitlen;           /* total message length in bits, pre-padding */
    uint8_t  buf[SHA256_BLOCK_SIZE];
    uint32_t buf_len;          /* bytes currently buffered (0..63) */
};

void sha256_init(struct sha256_ctx *ctx);
void sha256_update(struct sha256_ctx *ctx, const void *data, size_t len);
void sha256_final(struct sha256_ctx *ctx, uint8_t out[SHA256_DIGEST_SIZE]);

/* One-shot convenience wrapper. */
void sha256(const void *data, size_t len, uint8_t out[SHA256_DIGEST_SIZE]);

/* Known-answer tests (empty string, "abc", NIST's two-block message) against
 * digests independently computed via Python's hashlib -- not just internal
 * round-trip self-consistency. */
int sha256_run_selftests(void);

#endif /* _EMBK_CRYPTO_SHA256_H */
