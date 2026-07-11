#include "crypto/sha256.h"
#include "include/types.h"
#include "include/kstring.h"
#include "include/kprintf.h"

/* FIPS 180-4 round constants: the fractional parts of the cube roots of the
 * first 64 primes. Extremely standard, but errors here are invisible until
 * checked against an oracle -- see sha256_run_selftests(). */
static const uint32_t K[64] = {
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
    0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
    0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
    0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
    0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
    0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
};

static inline uint32_t rotr32(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32 - n));
}

static void sha256_transform(struct sha256_ctx *ctx, const uint8_t block[SHA256_BLOCK_SIZE]) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i*4] << 24) | ((uint32_t)block[i*4+1] << 16) |
               ((uint32_t)block[i*4+2] << 8)  | ((uint32_t)block[i*4+3]);
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr32(w[i-15], 7) ^ rotr32(w[i-15], 18) ^ (w[i-15] >> 3);
        uint32_t s1 = rotr32(w[i-2], 17) ^ rotr32(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }

    uint32_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2], d = ctx->state[3];
    uint32_t e = ctx->state[4], f = ctx->state[5], g = ctx->state[6], h = ctx->state[7];

    for (int i = 0; i < 64; i++) {
        uint32_t S1 = rotr32(e,6) ^ rotr32(e,11) ^ rotr32(e,25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t temp1 = h + S1 + ch + K[i] + w[i];
        uint32_t S0 = rotr32(a,2) ^ rotr32(a,13) ^ rotr32(a,22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = S0 + maj;

        h = g; g = f; f = e; e = d + temp1;
        d = c; c = b; b = a; a = temp1 + temp2;
    }

    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

void sha256_init(struct sha256_ctx *ctx) {
    ctx->state[0] = 0x6a09e667u; ctx->state[1] = 0xbb67ae85u;
    ctx->state[2] = 0x3c6ef372u; ctx->state[3] = 0xa54ff53au;
    ctx->state[4] = 0x510e527fu; ctx->state[5] = 0x9b05688cu;
    ctx->state[6] = 0x1f83d9abu; ctx->state[7] = 0x5be0cd19u;
    ctx->bitlen = 0;
    ctx->buf_len = 0;
}

void sha256_update(struct sha256_ctx *ctx, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    while (len > 0) {
        size_t take = SHA256_BLOCK_SIZE - ctx->buf_len;
        if (take > len) take = len;
        memcpy(ctx->buf + ctx->buf_len, p, take);
        ctx->buf_len += (uint32_t)take;
        p += take;
        len -= take;
        ctx->bitlen += (uint64_t)take * 8;
        if (ctx->buf_len == SHA256_BLOCK_SIZE) {
            sha256_transform(ctx, ctx->buf);
            ctx->buf_len = 0;
        }
    }
}

void sha256_final(struct sha256_ctx *ctx, uint8_t out[SHA256_DIGEST_SIZE]) {
    /* Capture the true message length (in bits) before padding -- the loop
     * below keeps feeding sha256_update(), which keeps bumping ctx->bitlen,
     * but the length field appended at the end must reflect the ORIGINAL
     * message, not message+padding. */
    uint64_t bitlen = ctx->bitlen;

    uint8_t pad = 0x80;
    sha256_update(ctx, &pad, 1);

    uint8_t zero = 0;
    while (ctx->buf_len != 56) {
        sha256_update(ctx, &zero, 1);
    }

    uint8_t lenbytes[8];
    for (int i = 0; i < 8; i++) {
        lenbytes[i] = (uint8_t)(bitlen >> (56 - 8 * i));
    }
    sha256_update(ctx, lenbytes, 8);

    for (int i = 0; i < 8; i++) {
        out[i*4]   = (uint8_t)(ctx->state[i] >> 24);
        out[i*4+1] = (uint8_t)(ctx->state[i] >> 16);
        out[i*4+2] = (uint8_t)(ctx->state[i] >> 8);
        out[i*4+3] = (uint8_t)(ctx->state[i]);
    }
}

void sha256(const void *data, size_t len, uint8_t out[SHA256_DIGEST_SIZE]) {
    struct sha256_ctx ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, data, len);
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

/* Vectors independently computed via `python3 -c "import hashlib; ..."`
 * (hashlib.sha256), not transcribed from memory -- see the v2.2 crypto
 * phase notes in docs/EMBKFS_spec_v2.2.md for how these were generated. */
int sha256_run_selftests(void) {
    uint8_t out[SHA256_DIGEST_SIZE];
    bool ok = true;

    sha256("", 0, out);
    if (!digest_eq_hex(out, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")) {
        kprintf("CRYPTO: sha256: FAIL empty-string vector\n");
        ok = false;
    }

    sha256("abc", 3, out);
    if (!digest_eq_hex(out, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")) {
        kprintf("CRYPTO: sha256: FAIL 'abc' vector\n");
        ok = false;
    }

    static const char two_block[] = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha256(two_block, sizeof(two_block) - 1, out);
    if (!digest_eq_hex(out, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1")) {
        kprintf("CRYPTO: sha256: FAIL two-block vector\n");
        ok = false;
    }

    /* Streaming vs one-shot must agree -- exercises the buffer/carry logic
     * in sha256_update() across an arbitrary split point. */
    struct sha256_ctx ctx;
    uint8_t streamed[SHA256_DIGEST_SIZE];
    sha256_init(&ctx);
    sha256_update(&ctx, two_block, 23);
    sha256_update(&ctx, two_block + 23, sizeof(two_block) - 1 - 23);
    sha256_final(&ctx, streamed);
    if (memcmp(streamed, out, SHA256_DIGEST_SIZE) != 0) {
        kprintf("CRYPTO: sha256: FAIL streaming/one-shot mismatch\n");
        ok = false;
    }

    kprintf("CRYPTO: sha256: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : -1;
}
