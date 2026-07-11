#ifndef _EMBK_CRYPTO_HMAC_H
#define _EMBK_CRYPTO_HMAC_H

#include <stdint.h>
#include <stddef.h>
#include "crypto/sha256.h"

/* HMAC-SHA256 (RFC 2104 construction over SHA-256). Used directly by the
 * verified-root boot check (Phase 5d) and as the PRF underneath
 * PBKDF2-HMAC-SHA256 (pbkdf2.h, Phase 4's passphrase KDF). */
void hmac_sha256(const uint8_t *key, size_t key_len,
                  const uint8_t *msg, size_t msg_len,
                  uint8_t out[SHA256_DIGEST_SIZE]);

/* RFC 4231 test case 1 against a digest independently computed via
 * Python's hmac module. */
int hmac_sha256_run_selftests(void);

#endif /* _EMBK_CRYPTO_HMAC_H */
