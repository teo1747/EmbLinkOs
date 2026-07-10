#ifndef _EMBK_CRYPTO_PBKDF2_H
#define _EMBK_CRYPTO_PBKDF2_H

#include <stdint.h>
#include <stddef.h>

/* PBKDF2-HMAC-SHA256 (RFC 8018) -- the passphrase KDF for Phase 4's
 * mount-time unlock flow. Salts are expected to be small (EMBKFS's on-disk
 * KDF salt is 16 bytes); PBKDF2_MAX_SALT_LEN bounds the internal working
 * buffer and is generous headroom above that, not a general-purpose limit. */
#define PBKDF2_MAX_SALT_LEN 64

void pbkdf2_hmac_sha256(const uint8_t *password, size_t password_len,
                         const uint8_t *salt, size_t salt_len,
                         uint32_t iterations,
                         uint8_t *out, size_t out_len);

/* Verifies against digests independently computed via Python's
 * hashlib.pbkdf2_hmac(), plus a definitional cross-check (1-iteration
 * PBKDF2 output must equal a single direct HMAC computation) that holds
 * by construction regardless of any external vector. */
int pbkdf2_run_selftests(void);

#endif /* _EMBK_CRYPTO_PBKDF2_H */
