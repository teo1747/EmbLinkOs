#include "crypto/pbkdf2.h"
#include "crypto/hmac.h"
#include "include/types.h"
#include "include/kstring.h"
#include "include/kprintf.h"

void pbkdf2_hmac_sha256(const uint8_t *password, size_t password_len,
                         const uint8_t *salt, size_t salt_len,
                         uint32_t iterations,
                         uint8_t *out, size_t out_len) {
    if (salt_len > PBKDF2_MAX_SALT_LEN) salt_len = PBKDF2_MAX_SALT_LEN;
    if (iterations == 0) iterations = 1;

    uint8_t salt_block[PBKDF2_MAX_SALT_LEN + 4];
    memcpy(salt_block, salt, salt_len);

    uint8_t *dst = out;
    size_t remaining = out_len;
    uint32_t block_index = 1;

    while (remaining > 0) {
        /* salt || INT32_BE(block_index), per RFC 8018's F() function. */
        salt_block[salt_len + 0] = (uint8_t)(block_index >> 24);
        salt_block[salt_len + 1] = (uint8_t)(block_index >> 16);
        salt_block[salt_len + 2] = (uint8_t)(block_index >> 8);
        salt_block[salt_len + 3] = (uint8_t)(block_index);

        uint8_t u[SHA256_DIGEST_SIZE];
        uint8_t t[SHA256_DIGEST_SIZE];

        hmac_sha256(password, password_len, salt_block, salt_len + 4, u);
        memcpy(t, u, SHA256_DIGEST_SIZE);

        for (uint32_t iter = 1; iter < iterations; iter++) {
            hmac_sha256(password, password_len, u, SHA256_DIGEST_SIZE, u);
            for (int i = 0; i < SHA256_DIGEST_SIZE; i++) t[i] ^= u[i];
        }

        size_t take = remaining < SHA256_DIGEST_SIZE ? remaining : SHA256_DIGEST_SIZE;
        memcpy(dst, t, take);
        dst += take;
        remaining -= take;
        block_index++;
    }
}

static bool bytes_eq_hex(const uint8_t *bytes, size_t len, const char *hex) {
    for (size_t i = 0; i < len; i++) {
        char hi = hex[i*2], lo = hex[i*2+1];
        uint8_t hb = (hi <= '9') ? (uint8_t)(hi - '0') : (uint8_t)(hi - 'a' + 10);
        uint8_t lb = (lo <= '9') ? (uint8_t)(lo - '0') : (uint8_t)(lo - 'a' + 10);
        if (bytes[i] != ((hb << 4) | lb)) return false;
    }
    return true;
}

int pbkdf2_run_selftests(void) {
    bool ok = true;
    uint8_t out[32];

    /* Independently computed via
     * hashlib.pbkdf2_hmac('sha256', b'password', b'salt', 1, 32) and
     * ...4096, 32). */
    pbkdf2_hmac_sha256((const uint8_t *)"password", 8, (const uint8_t *)"salt", 4, 1, out, 32);
    if (!bytes_eq_hex(out, 32, "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b")) {
        kprintf("CRYPTO: pbkdf2: FAIL c=1 vector\n");
        ok = false;
    }

    pbkdf2_hmac_sha256((const uint8_t *)"password", 8, (const uint8_t *)"salt", 4, 4096, out, 32);
    if (!bytes_eq_hex(out, 32, "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a")) {
        kprintf("CRYPTO: pbkdf2: FAIL c=4096 vector\n");
        ok = false;
    }

    /* Definitional cross-check, independent of any external vector: with
     * exactly 1 iteration, PBKDF2's F() is nothing but a single HMAC over
     * salt||INT32_BE(1) -- true by RFC 8018's own definition, not just an
     * implementation detail, so this catches block-index/loop-init bugs
     * even if the external vector above were ever wrong. */
    uint8_t salt_plus_index[8] = { 's', 'a', 'l', 't', 0, 0, 0, 1 };
    uint8_t direct[SHA256_DIGEST_SIZE];
    hmac_sha256((const uint8_t *)"password", 8, salt_plus_index, sizeof salt_plus_index, direct);
    uint8_t via_pbkdf2[SHA256_DIGEST_SIZE];
    pbkdf2_hmac_sha256((const uint8_t *)"password", 8, (const uint8_t *)"salt", 4, 1, via_pbkdf2, SHA256_DIGEST_SIZE);
    if (memcmp(direct, via_pbkdf2, SHA256_DIGEST_SIZE) != 0) {
        kprintf("CRYPTO: pbkdf2: FAIL c=1 does not equal a direct HMAC computation\n");
        ok = false;
    }

    kprintf("CRYPTO: pbkdf2: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : -1;
}
