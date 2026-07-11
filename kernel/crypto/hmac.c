#include "crypto/hmac.h"
#include "include/types.h"
#include "include/kstring.h"
#include "include/kprintf.h"

void hmac_sha256(const uint8_t *key, size_t key_len,
                  const uint8_t *msg, size_t msg_len,
                  uint8_t out[SHA256_DIGEST_SIZE]) {
    uint8_t key_block[SHA256_BLOCK_SIZE];
    memset(key_block, 0, sizeof key_block);

    /* RFC 2104: keys longer than the block size are hashed down first;
     * shorter keys are zero-padded (memset above already did the padding). */
    if (key_len > SHA256_BLOCK_SIZE) {
        sha256(key, key_len, key_block);
    } else {
        memcpy(key_block, key, key_len);
    }

    uint8_t ipad[SHA256_BLOCK_SIZE], opad[SHA256_BLOCK_SIZE];
    for (int i = 0; i < SHA256_BLOCK_SIZE; i++) {
        ipad[i] = (uint8_t)(key_block[i] ^ 0x36);
        opad[i] = (uint8_t)(key_block[i] ^ 0x5c);
    }

    struct sha256_ctx ctx;
    uint8_t inner[SHA256_DIGEST_SIZE];
    sha256_init(&ctx);
    sha256_update(&ctx, ipad, sizeof ipad);
    sha256_update(&ctx, msg, msg_len);
    sha256_final(&ctx, inner);

    sha256_init(&ctx);
    sha256_update(&ctx, opad, sizeof opad);
    sha256_update(&ctx, inner, sizeof inner);
    sha256_final(&ctx, out);
}

static bool digest_eq_hex(const uint8_t digest[SHA256_DIGEST_SIZE], const char *hex) {
    for (int i = 0; i < SHA256_DIGEST_SIZE; i++) {
        char hi = hex[i*2], lo = hex[i*2+1];
        uint8_t hb = (hi <= '9') ? (uint8_t)(hi - '0') : (uint8_t)(hi - 'a' + 10);
        uint8_t lb = (lo <= '9') ? (uint8_t)(lo - '0') : (uint8_t)(lo - 'a' + 10);
        if (digest[i] != ((hb << 4) | lb)) return false;
    }
    return true;
}

/* RFC 4231 test case 1: key = 0x0b * 20, data = "Hi There". Expected digest
 * independently computed via `python3 -c "import hmac,hashlib; ..."`. */
int hmac_sha256_run_selftests(void) {
    uint8_t key[20];
    memset(key, 0x0b, sizeof key);
    uint8_t out[SHA256_DIGEST_SIZE];
    hmac_sha256(key, sizeof key, (const uint8_t *)"Hi There", 8, out);

    bool ok = digest_eq_hex(out, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
    if (!ok) {
        kprintf("CRYPTO: hmac_sha256: FAIL RFC4231 test case 1\n");
    }
    kprintf("CRYPTO: hmac_sha256: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : -1;
}
