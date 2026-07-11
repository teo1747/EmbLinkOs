#include "crypto/xts.h"
#include "include/types.h"
#include "include/kstring.h"
#include "include/kprintf.h"

void aes_xts_init(struct aes_xts_ctx *ctx,
                   const uint8_t data_key[AES256_KEY_SIZE],
                   const uint8_t tweak_key[AES256_KEY_SIZE]) {
    aes256_init(&ctx->data, data_key);
    aes256_init(&ctx->tweak, tweak_key);
}

/* Multiply the 128-bit tweak by alpha (the primitive element) in
 * GF(2^128) with reduction polynomial x^128+x^7+x^2+x+1 (0x87) -- the
 * standard XTS tweak-advance step. Tweak bytes are little-endian (byte 0
 * is the least-significant byte), per IEEE P1619. */
static void gf128_mul_alpha(uint8_t t[16]) {
    uint8_t carry = 0;
    for (int i = 0; i < 16; i++) {
        uint8_t new_carry = (uint8_t)(t[i] >> 7) & 1;
        t[i] = (uint8_t)((t[i] << 1) | carry);
        carry = new_carry;
    }
    if (carry) t[0] = (uint8_t)(t[0] ^ 0x87);
}

static void block_number_to_tweak_input(uint64_t block_number, uint8_t tweak_in[16]) {
    for (int i = 0; i < 8; i++) tweak_in[i] = (uint8_t)(block_number >> (8 * i));
    memset(tweak_in + 8, 0, 8);
}

void aes_xts_encrypt(const struct aes_xts_ctx *ctx, uint64_t block_number,
                      const uint8_t *in, uint8_t *out, size_t len) {
    uint8_t tweak_in[16];
    block_number_to_tweak_input(block_number, tweak_in);

    uint8_t t[16];
    aes256_encrypt_block(&ctx->tweak, tweak_in, t);

    for (size_t off = 0; off + AES_BLOCK_SIZE <= len; off += AES_BLOCK_SIZE) {
        uint8_t x[16];
        for (int i = 0; i < 16; i++) x[i] = (uint8_t)(in[off + i] ^ t[i]);
        uint8_t y[16];
        aes256_encrypt_block(&ctx->data, x, y);
        for (int i = 0; i < 16; i++) out[off + i] = (uint8_t)(y[i] ^ t[i]);
        gf128_mul_alpha(t);
    }
}

void aes_xts_decrypt(const struct aes_xts_ctx *ctx, uint64_t block_number,
                      const uint8_t *in, uint8_t *out, size_t len) {
    uint8_t tweak_in[16];
    block_number_to_tweak_input(block_number, tweak_in);

    uint8_t t[16];
    aes256_encrypt_block(&ctx->tweak, tweak_in, t);

    for (size_t off = 0; off + AES_BLOCK_SIZE <= len; off += AES_BLOCK_SIZE) {
        uint8_t x[16];
        for (int i = 0; i < 16; i++) x[i] = (uint8_t)(in[off + i] ^ t[i]);
        uint8_t y[16];
        aes256_decrypt_block(&ctx->data, x, y);
        for (int i = 0; i < 16; i++) out[off + i] = (uint8_t)(y[i] ^ t[i]);
        gf128_mul_alpha(t);
    }
}

static void hex_to_bytes(const char *hex, uint8_t *out, size_t len) {
    for (size_t i = 0; i < len; i++) {
        char hi = hex[i*2], lo = hex[i*2+1];
        uint8_t hb = (hi <= '9') ? (uint8_t)(hi - '0') : (uint8_t)(hi - 'a' + 10);
        uint8_t lb = (lo <= '9') ? (uint8_t)(lo - '0') : (uint8_t)(lo - 'a' + 10);
        out[i] = (uint8_t)((hb << 4) | lb);
    }
}

int aes_xts_run_selftests(void) {
    bool ok = true;

    uint8_t data_key[32], tweak_key[32];
    for (int i = 0; i < 32; i++) data_key[i] = (uint8_t)i;
    for (int i = 0; i < 32; i++) tweak_key[i] = (uint8_t)(0x20 + i);

    uint8_t plaintext[64];
    for (int i = 0; i < 64; i++) plaintext[i] = (uint8_t)i;

    struct aes_xts_ctx ctx;
    aes_xts_init(&ctx, data_key, tweak_key);

    /* 4-AES-block (64-byte) known-answer vectors, independently computed
     * via Python's `cryptography` package (algorithms.AES(data_key ||
     * tweak_key), modes.XTS(tweak)) with this exact little-endian
     * block-number tweak encoding. */
    struct { uint64_t block_number; const char *ct_hex; } kats[] = {
        { 0,          "dc8c665b97cbc0246d4f1639a9678a3e2a2dcf4a3fbf1342ebbb771234f1a1c3cb885182e54e277aa90875bfb779b27c28568d2731fd61d0b43248046597e326" },
        { 1,          "0976f139b289f2dd570e3b8caa596f98f86a162f8768ffbd7ad06c74d403f32a91c4b978ee3dac956ffbabf0026ad4b86e37008a99d93dad13148410a54e9816" },
        { 255,        "75de381013f2a09b6655cf5e407ca71ccb3623e1ed6ffb5447b872c185f29eacb08899466f3f33212f09ba249f6b1eba772f26e2417d80acefeec0fb9c6fb48a" },
        { 4328719365ULL, "5a034815566a037d215c27761217ec7ab90c5393e91fdc3652c8c82b7d4b6f1ab8007362aa7140696bb82f3a3b28a9f06f8bf22aadc527b825b21b991ba0e7b4" },
    };

    for (size_t k = 0; k < sizeof(kats) / sizeof(kats[0]); k++) {
        uint8_t expected[64];
        hex_to_bytes(kats[k].ct_hex, expected, 64);

        uint8_t ct[64];
        aes_xts_encrypt(&ctx, kats[k].block_number, plaintext, ct, 64);
        if (memcmp(ct, expected, 64) != 0) {
            kprintf("CRYPTO: xts: FAIL known-answer vector for block %lu\n", (unsigned long)kats[k].block_number);
            ok = false;
        }

        uint8_t pt2[64];
        aes_xts_decrypt(&ctx, kats[k].block_number, ct, pt2, 64);
        if (memcmp(pt2, plaintext, 64) != 0) {
            kprintf("CRYPTO: xts: FAIL decrypt(encrypt(x)) != x for block %lu\n", (unsigned long)kats[k].block_number);
            ok = false;
        }
    }

    /* Differential checks: changing the block number (tweak) must change
     * the ciphertext even for identical plaintext -- catches "tweak
     * ignored" bugs that round-trip tests alone would miss. */
    uint8_t ct_a[64], ct_b[64];
    aes_xts_encrypt(&ctx, 0, plaintext, ct_a, 64);
    aes_xts_encrypt(&ctx, 1, plaintext, ct_b, 64);
    if (memcmp(ct_a, ct_b, 64) == 0) {
        kprintf("CRYPTO: xts: FAIL ciphertext independent of block number\n");
        ok = false;
    }

    /* One byte of plaintext difference must change the WHOLE encrypted
     * AES block it falls in (not just propagate additively), and must not
     * change any OTHER block's ciphertext within the same sector. */
    uint8_t plaintext2[64];
    memcpy(plaintext2, plaintext, 64);
    plaintext2[0] ^= 0x01;
    uint8_t ct_c[64];
    aes_xts_encrypt(&ctx, 0, plaintext2, ct_c, 64);
    if (memcmp(ct_c, ct_a, 16) == 0) {
        kprintf("CRYPTO: xts: FAIL first block ciphertext unchanged despite plaintext change\n");
        ok = false;
    }
    if (memcmp(ct_c + 16, ct_a + 16, 48) != 0) {
        kprintf("CRYPTO: xts: FAIL a change in block 0's plaintext altered other blocks' ciphertext\n");
        ok = false;
    }

    kprintf("CRYPTO: xts: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : -1;
}
