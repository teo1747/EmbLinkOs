#include "aes.h"
#include "../include/types.h"
#include "../include/kstring.h"
#include "../include/kprintf.h"

/* S-box and its inverse, generated from first principles (GF(2^8)
 * multiplicative inverse + the standard affine transform, modulus
 * x^8+x^4+x^3+x+1 / 0x11B) rather than transcribed from memory -- see the
 * generator script referenced in docs/EMBKFS_spec_v2.2.md. Values verified
 * against the canonical checks sbox[0x00]==0x63, sbox[0x53]==0xed. */
static const uint8_t sbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16,
};

static const uint8_t inv_sbox[256] = {
    0x52, 0x09, 0x6a, 0xd5, 0x30, 0x36, 0xa5, 0x38, 0xbf, 0x40, 0xa3, 0x9e, 0x81, 0xf3, 0xd7, 0xfb,
    0x7c, 0xe3, 0x39, 0x82, 0x9b, 0x2f, 0xff, 0x87, 0x34, 0x8e, 0x43, 0x44, 0xc4, 0xde, 0xe9, 0xcb,
    0x54, 0x7b, 0x94, 0x32, 0xa6, 0xc2, 0x23, 0x3d, 0xee, 0x4c, 0x95, 0x0b, 0x42, 0xfa, 0xc3, 0x4e,
    0x08, 0x2e, 0xa1, 0x66, 0x28, 0xd9, 0x24, 0xb2, 0x76, 0x5b, 0xa2, 0x49, 0x6d, 0x8b, 0xd1, 0x25,
    0x72, 0xf8, 0xf6, 0x64, 0x86, 0x68, 0x98, 0x16, 0xd4, 0xa4, 0x5c, 0xcc, 0x5d, 0x65, 0xb6, 0x92,
    0x6c, 0x70, 0x48, 0x50, 0xfd, 0xed, 0xb9, 0xda, 0x5e, 0x15, 0x46, 0x57, 0xa7, 0x8d, 0x9d, 0x84,
    0x90, 0xd8, 0xab, 0x00, 0x8c, 0xbc, 0xd3, 0x0a, 0xf7, 0xe4, 0x58, 0x05, 0xb8, 0xb3, 0x45, 0x06,
    0xd0, 0x2c, 0x1e, 0x8f, 0xca, 0x3f, 0x0f, 0x02, 0xc1, 0xaf, 0xbd, 0x03, 0x01, 0x13, 0x8a, 0x6b,
    0x3a, 0x91, 0x11, 0x41, 0x4f, 0x67, 0xdc, 0xea, 0x97, 0xf2, 0xcf, 0xce, 0xf0, 0xb4, 0xe6, 0x73,
    0x96, 0xac, 0x74, 0x22, 0xe7, 0xad, 0x35, 0x85, 0xe2, 0xf9, 0x37, 0xe8, 0x1c, 0x75, 0xdf, 0x6e,
    0x47, 0xf1, 0x1a, 0x71, 0x1d, 0x29, 0xc5, 0x89, 0x6f, 0xb7, 0x62, 0x0e, 0xaa, 0x18, 0xbe, 0x1b,
    0xfc, 0x56, 0x3e, 0x4b, 0xc6, 0xd2, 0x79, 0x20, 0x9a, 0xdb, 0xc0, 0xfe, 0x78, 0xcd, 0x5a, 0xf4,
    0x1f, 0xdd, 0xa8, 0x33, 0x88, 0x07, 0xc7, 0x31, 0xb1, 0x12, 0x10, 0x59, 0x27, 0x80, 0xec, 0x5f,
    0x60, 0x51, 0x7f, 0xa9, 0x19, 0xb5, 0x4a, 0x0d, 0x2d, 0xe5, 0x7a, 0x9f, 0x93, 0xc9, 0x9c, 0xef,
    0xa0, 0xe0, 0x3b, 0x4d, 0xae, 0x2a, 0xf5, 0xb0, 0xc8, 0xeb, 0xbb, 0x3c, 0x83, 0x53, 0x99, 0x61,
    0x17, 0x2b, 0x04, 0x7e, 0xba, 0x77, 0xd6, 0x26, 0xe1, 0x69, 0x14, 0x63, 0x55, 0x21, 0x0c, 0x7d,
};

/* Round constants for key expansion (Rcon[i/Nk] as a single byte -- the
 * high byte of the conventional 32-bit Rcon word, low 3 bytes are always
 * zero so we only ever need the byte form). Index 0 is unused. */
static const uint8_t rcon[15] = {
    0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40,
    0x80, 0x1b, 0x36, 0x6c, 0xd8, 0xab, 0x4d /* AES-256 only ever indexes up through [7] */
};

static inline uint8_t xtime(uint8_t x) {
    return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1b : 0x00));
}

/* Generic GF(2^8) multiply (double-and-add), used by Mix/InvMixColumns. */
static uint8_t gmul(uint8_t a, uint8_t b) {
    uint8_t p = 0;
    for (int i = 0; i < 8; i++) {
        if (b & 1) p ^= a;
        a = xtime(a);
        b >>= 1;
    }
    return p;
}

#define AES256_NK 8   /* key length in 32-bit words */
#define AES_NB    4   /* block size in 32-bit words (always 4 for AES) */

void aes256_init(struct aes256_ctx *ctx, const uint8_t key[AES256_KEY_SIZE]) {
    uint8_t *rk = ctx->round_keys; /* AES_NB*(AES256_NR+1) words, 4 bytes each, laid out flat */
    memcpy(rk, key, AES256_KEY_SIZE);

    int total_words = AES_NB * (AES256_NR + 1); /* 60 */
    for (int i = AES256_NK; i < total_words; i++) {
        uint8_t temp[4];
        memcpy(temp, rk + (i - 1) * 4, 4);

        if (i % AES256_NK == 0) {
            /* RotWord */
            uint8_t t0 = temp[0];
            temp[0] = temp[1]; temp[1] = temp[2]; temp[2] = temp[3]; temp[3] = t0;
            /* SubWord */
            for (int k = 0; k < 4; k++) temp[k] = sbox[temp[k]];
            temp[0] = (uint8_t)(temp[0] ^ rcon[i / AES256_NK]);
        } else if (i % AES256_NK == 4) {
            /* AES-256 only: an extra SubWord at the Nk/2 offset. */
            for (int k = 0; k < 4; k++) temp[k] = sbox[temp[k]];
        }

        for (int k = 0; k < 4; k++) {
            rk[i * 4 + k] = (uint8_t)(rk[(i - AES256_NK) * 4 + k] ^ temp[k]);
        }
    }
}

/* state[row][col] is stored as state[row + 4*col], matching FIPS-197's
 * column-major byte ordering (bytes 0-3 of the input fill column 0, etc). */

static void add_round_key(uint8_t state[16], const uint8_t *rk) {
    for (int i = 0; i < 16; i++) state[i] = (uint8_t)(state[i] ^ rk[i]);
}

static void sub_bytes(uint8_t state[16]) {
    for (int i = 0; i < 16; i++) state[i] = sbox[state[i]];
}

static void inv_sub_bytes(uint8_t state[16]) {
    for (int i = 0; i < 16; i++) state[i] = inv_sbox[state[i]];
}

static void shift_rows(uint8_t state[16]) {
    uint8_t t;
    /* row 1: shift left 1 */
    t = state[1]; state[1] = state[5]; state[5] = state[9]; state[9] = state[13]; state[13] = t;
    /* row 2: shift left 2 */
    t = state[2]; state[2] = state[10]; state[10] = t;
    t = state[6]; state[6] = state[14]; state[14] = t;
    /* row 3: shift left 3 (== shift right 1) */
    t = state[15]; state[15] = state[11]; state[11] = state[7]; state[7] = state[3]; state[3] = t;
}

static void inv_shift_rows(uint8_t state[16]) {
    uint8_t t;
    /* row 1: shift right 1 */
    t = state[13]; state[13] = state[9]; state[9] = state[5]; state[5] = state[1]; state[1] = t;
    /* row 2: shift right 2 (same as left 2) */
    t = state[2]; state[2] = state[10]; state[10] = t;
    t = state[6]; state[6] = state[14]; state[14] = t;
    /* row 3: shift right 3 (== shift left 1) */
    t = state[3]; state[3] = state[7]; state[7] = state[11]; state[11] = state[15]; state[15] = t;
}

static void mix_columns(uint8_t state[16]) {
    for (int c = 0; c < 4; c++) {
        uint8_t *s = state + c * 4;
        uint8_t a0 = s[0], a1 = s[1], a2 = s[2], a3 = s[3];
        s[0] = (uint8_t)(gmul(a0,2) ^ gmul(a1,3) ^ a2 ^ a3);
        s[1] = (uint8_t)(a0 ^ gmul(a1,2) ^ gmul(a2,3) ^ a3);
        s[2] = (uint8_t)(a0 ^ a1 ^ gmul(a2,2) ^ gmul(a3,3));
        s[3] = (uint8_t)(gmul(a0,3) ^ a1 ^ a2 ^ gmul(a3,2));
    }
}

static void inv_mix_columns(uint8_t state[16]) {
    for (int c = 0; c < 4; c++) {
        uint8_t *s = state + c * 4;
        uint8_t a0 = s[0], a1 = s[1], a2 = s[2], a3 = s[3];
        s[0] = (uint8_t)(gmul(a0,14) ^ gmul(a1,11) ^ gmul(a2,13) ^ gmul(a3,9));
        s[1] = (uint8_t)(gmul(a0,9)  ^ gmul(a1,14) ^ gmul(a2,11) ^ gmul(a3,13));
        s[2] = (uint8_t)(gmul(a0,13) ^ gmul(a1,9)  ^ gmul(a2,14) ^ gmul(a3,11));
        s[3] = (uint8_t)(gmul(a0,11) ^ gmul(a1,13) ^ gmul(a2,9)  ^ gmul(a3,14));
    }
}

void aes256_encrypt_block(const struct aes256_ctx *ctx, const uint8_t in[AES_BLOCK_SIZE], uint8_t out[AES_BLOCK_SIZE]) {
    uint8_t state[16];
    memcpy(state, in, 16);

    add_round_key(state, ctx->round_keys);
    for (int round = 1; round < AES256_NR; round++) {
        sub_bytes(state);
        shift_rows(state);
        mix_columns(state);
        add_round_key(state, ctx->round_keys + round * 16);
    }
    sub_bytes(state);
    shift_rows(state);
    add_round_key(state, ctx->round_keys + AES256_NR * 16);

    memcpy(out, state, 16);
}

void aes256_decrypt_block(const struct aes256_ctx *ctx, const uint8_t in[AES_BLOCK_SIZE], uint8_t out[AES_BLOCK_SIZE]) {
    uint8_t state[16];
    memcpy(state, in, 16);

    add_round_key(state, ctx->round_keys + AES256_NR * 16);
    for (int round = AES256_NR - 1; round >= 1; round--) {
        inv_shift_rows(state);
        inv_sub_bytes(state);
        add_round_key(state, ctx->round_keys + round * 16);
        inv_mix_columns(state);
    }
    inv_shift_rows(state);
    inv_sub_bytes(state);
    add_round_key(state, ctx->round_keys);

    memcpy(out, state, 16);
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

static void hex_to_bytes(const char *hex, uint8_t *out, size_t len) {
    for (size_t i = 0; i < len; i++) {
        char hi = hex[i*2], lo = hex[i*2+1];
        uint8_t hb = (hi <= '9') ? (uint8_t)(hi - '0') : (uint8_t)(hi - 'a' + 10);
        uint8_t lb = (lo <= '9') ? (uint8_t)(lo - '0') : (uint8_t)(lo - 'a' + 10);
        out[i] = (uint8_t)((hb << 4) | lb);
    }
}

/* FIPS-197 Appendix C.3, cross-checked against Python's `cryptography`
 * package (algorithms.AES(key), modes.ECB()) -- both agree. */
int aes256_run_selftests(void) {
    bool ok = true;

    uint8_t key[32], pt[16], expected_ct[16];
    hex_to_bytes("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f", key, 32);
    hex_to_bytes("00112233445566778899aabbccddeeff", pt, 16);
    hex_to_bytes("8ea2b7ca516745bfeafc49904b496089", expected_ct, 16);

    struct aes256_ctx ctx;
    aes256_init(&ctx, key);

    uint8_t ct[16];
    aes256_encrypt_block(&ctx, pt, ct);
    if (memcmp(ct, expected_ct, 16) != 0) {
        kprintf("CRYPTO: aes256: FAIL FIPS-197 C.3 encrypt vector\n");
        ok = false;
    }
    if (!bytes_eq_hex(ct, 16, "8ea2b7ca516745bfeafc49904b496089")) {
        kprintf("CRYPTO: aes256: FAIL hex-form re-check of C.3 vector\n");
        ok = false;
    }

    uint8_t pt2[16];
    aes256_decrypt_block(&ctx, ct, pt2);
    if (memcmp(pt2, pt, 16) != 0) {
        kprintf("CRYPTO: aes256: FAIL decrypt(encrypt(x)) != x\n");
        ok = false;
    }

    /* Broader round-trip sweep across varied keys/plaintexts, catching bugs
     * the single KAT's specific byte values might not exercise. */
    for (uint32_t trial = 0; trial < 64 && ok; trial++) {
        uint8_t k2[32], p2[16], c2[16], p3[16];
        for (int i = 0; i < 32; i++) k2[i] = (uint8_t)(trial * 7 + i * 13 + 1);
        for (int i = 0; i < 16; i++) p2[i] = (uint8_t)(trial * 3 + i * 5 + 1);
        struct aes256_ctx ctx2;
        aes256_init(&ctx2, k2);
        aes256_encrypt_block(&ctx2, p2, c2);
        aes256_decrypt_block(&ctx2, c2, p3);
        if (memcmp(p2, p3, 16) != 0) {
            kprintf("CRYPTO: aes256: FAIL round-trip sweep at trial %u\n", (unsigned int)trial);
            ok = false;
        }
        if (memcmp(p2, c2, 16) == 0) {
            kprintf("CRYPTO: aes256: FAIL ciphertext equals plaintext at trial %u (no-op cipher?)\n", (unsigned int)trial);
            ok = false;
        }
    }

    kprintf("CRYPTO: aes256: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : -1;
}
