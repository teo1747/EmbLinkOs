#ifndef _EMBK_CRYPTO_AES_H
#define _EMBK_CRYPTO_AES_H

#include <stdint.h>

/*
 * AES-256 (FIPS-197), single-block ECB primitive only -- EMBKFS v2 never
 * uses AES in ECB mode directly for data; this is the building block
 * xts.h's AES-256-XTS wraps (two independent AES-256 keys: one for data,
 * one for tweak encryption). No AES-128/192 support -- the format only
 * ever calls for 256-bit keys, so there's no general-key-size machinery
 * to get subtly wrong.
 */

#define AES256_KEY_SIZE 32
#define AES_BLOCK_SIZE  16
#define AES256_NR       14   /* number of rounds for a 256-bit key */

struct aes256_ctx {
    /* Nr+1 round keys, 16 bytes (4 words) each. */
    uint8_t round_keys[(AES256_NR + 1) * AES_BLOCK_SIZE];
};

void aes256_init(struct aes256_ctx *ctx, const uint8_t key[AES256_KEY_SIZE]);
void aes256_encrypt_block(const struct aes256_ctx *ctx, const uint8_t in[AES_BLOCK_SIZE], uint8_t out[AES_BLOCK_SIZE]);
void aes256_decrypt_block(const struct aes256_ctx *ctx, const uint8_t in[AES_BLOCK_SIZE], uint8_t out[AES_BLOCK_SIZE]);

/* FIPS-197 Appendix C.3 known-answer vector, cross-checked against Python's
 * `cryptography` package (algorithms.AES + modes.ECB), not just typed from
 * memory. Also checks encrypt(decrypt(x)) == x round-trip. */
int aes256_run_selftests(void);

#endif /* _EMBK_CRYPTO_AES_H */
