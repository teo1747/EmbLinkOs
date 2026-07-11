#include "fs/embkfs/embkfs_compress.h"
#include "include/kstring.h"
#include "include/kprintf.h"
#include "include/kmalloc.h"

#define EMBKZ_MINMATCH    4u
#define EMBKZ_HASH_BITS   14
#define EMBKZ_HASH_SIZE   (1u << EMBKZ_HASH_BITS)
#define EMBKZ_MAX_OFFSET  65535u
#define EMBKZ_MAX_MATCH   0x00FFFFFFu  /* generous cap; real limit is dst_cap/src_len anyway */

uint32_t embk_compress_bound(uint32_t src_len) {
    return src_len + (src_len / 255u) + 16u;
}

static inline uint32_t read_u32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint32_t hash4(uint32_t x) {
    return (x * 2654435761u) >> (32 - EMBKZ_HASH_BITS);
}

/* Appends a length value using LZ4-style 255-byte extension: emits 0xFF
 * bytes while len >= 255 (subtracting 255 each time), then a final byte
 * with whatever remains (0..254). The reader reverses this by summing
 * bytes until one is read that isn't 255. */
static void emit_length_ext(uint8_t **dst, uint32_t len) {
    while (len >= 255) {
        *(*dst)++ = 255;
        len -= 255;
    }
    *(*dst)++ = (uint8_t)len;
}

static uint32_t match_length(const uint8_t *a, const uint8_t *b, uint32_t max) {
    uint32_t n = 0;
    while (n < max && a[n] == b[n]) n++;
    return n;
}

bool embk_compress(const uint8_t *src, uint32_t src_len,
                    uint8_t *dst, uint32_t dst_cap, uint32_t *out_len) {
    if (src_len < EMBKZ_MINMATCH + 1) return false; /* too small to ever help */

    int32_t *table = (int32_t *)kmalloc((uint64_t)EMBKZ_HASH_SIZE * sizeof(int32_t));
    if (!table) return false;
    for (uint32_t i = 0; i < EMBKZ_HASH_SIZE; i++) table[i] = -1;

    uint8_t *dst_start = dst;
    uint8_t *dst_end = dst + dst_cap;
    uint32_t pos = 0;
    uint32_t literal_start = 0;
    bool overflowed = false;

    while (pos + EMBKZ_MINMATCH <= src_len) {
        uint32_t h = hash4(read_u32le(src + pos));
        int32_t cand = table[h];
        table[h] = (int32_t)pos;

        uint32_t max_match = src_len - pos;
        if (max_match > EMBKZ_MAX_MATCH) max_match = EMBKZ_MAX_MATCH;

        uint32_t mlen = 0;
        uint32_t offset = 0;
        if (cand >= 0) {
            offset = pos - (uint32_t)cand;
            if (offset >= 1 && offset <= EMBKZ_MAX_OFFSET) {
                mlen = match_length(src + pos, src + (uint32_t)cand, max_match);
            }
        }

        if (mlen >= EMBKZ_MINMATCH) {
            uint32_t lit_len = pos - literal_start;
            uint32_t match_code = mlen - EMBKZ_MINMATCH;

            /* Reserve space for: token byte + literal extension worst case +
             * literal bytes + offset (2) + match extension worst case. */
            if (dst + 1 > dst_end) { overflowed = true; break; }
            uint8_t *token = dst++;
            uint8_t lit_nib = (uint8_t)(lit_len < 15 ? lit_len : 15);
            uint8_t match_nib = (uint8_t)(match_code < 15 ? match_code : 15);
            *token = (uint8_t)((lit_nib << 4) | match_nib);

            if (lit_len >= 15) {
                if (dst + ((lit_len - 15) / 255 + 1) > dst_end) { overflowed = true; break; }
                emit_length_ext(&dst, lit_len - 15);
            }
            if (dst + lit_len > dst_end) { overflowed = true; break; }
            memcpy(dst, src + literal_start, lit_len);
            dst += lit_len;

            /* A match ALWAYS needs its offset written, even when it happens
             * to consume the rest of the input -- the decoder only knows
             * "no match follows" by the LITERAL portion alone reaching the
             * expected output length (checked before it ever gets here);
             * once it's committed to reading a match section it must find
             * one. (Skipping this for an end-of-input match was a real bug,
             * caught by embk_compress_run_selftests()'s round-trip check.) */
            if (dst + 2 > dst_end) { overflowed = true; break; }
            *dst++ = (uint8_t)(offset & 0xFF);
            *dst++ = (uint8_t)((offset >> 8) & 0xFF);
            if (match_code >= 15) {
                if (dst + ((match_code - 15) / 255 + 1) > dst_end) { overflowed = true; break; }
                emit_length_ext(&dst, match_code - 15);
            }

            pos += mlen;
            literal_start = pos;

            /* Populate a couple of hash slots inside the match so future
             * matches can reference into it too (cheap, improves ratio on
             * repetitive data without a full rolling re-hash). */
            if (pos + EMBKZ_MINMATCH <= src_len) {
                table[hash4(read_u32le(src + pos - 1))] = (int32_t)(pos - 1);
            }
        } else {
            pos++;
        }
    }

    if (!overflowed) {
        /* Final literal-only sequence covering whatever's left (possibly 0). */
        uint32_t lit_len = src_len - literal_start;
        if (dst + 1 > dst_end) {
            overflowed = true;
        } else {
            uint8_t *token = dst++;
            uint8_t lit_nib = (uint8_t)(lit_len < 15 ? lit_len : 15);
            *token = (uint8_t)(lit_nib << 4); /* match nibble unused: this is the final sequence */
            if (lit_len >= 15) {
                if (dst + ((lit_len - 15) / 255 + 1) > dst_end) overflowed = true;
                else emit_length_ext(&dst, lit_len - 15);
            }
            if (!overflowed) {
                if (dst + lit_len > dst_end) overflowed = true;
                else { memcpy(dst, src + literal_start, lit_len); dst += lit_len; }
            }
        }
    }

    kfree(table);

    if (overflowed) return false;

    uint32_t produced = (uint32_t)(dst - dst_start);
    if (produced >= src_len) return false; /* didn't actually help */

    *out_len = produced;
    return true;
}

static uint32_t read_length_ext(const uint8_t **src, const uint8_t *src_end, bool *ok) {
    uint32_t total = 0;
    for (;;) {
        if (*src >= src_end) { *ok = false; return 0; }
        uint8_t b = *(*src)++;
        total += b;
        if (b != 255) break;
    }
    return total;
}

bool embk_decompress(const uint8_t *src, uint32_t src_len,
                      uint8_t *dst, uint32_t expected_len) {
    const uint8_t *sp = src;
    const uint8_t *send = src + src_len;
    uint8_t *dp = dst;
    uint8_t *dend = dst + expected_len;
    bool ok = true;

    while (dp < dend) {
        if (sp >= send) return false;
        uint8_t token = *sp++;
        uint32_t lit_len = (uint32_t)(token >> 4);
        uint32_t match_nib = (uint32_t)(token & 0x0F);

        if (lit_len == 15) {
            lit_len += read_length_ext(&sp, send, &ok);
            if (!ok) return false;
        }
        if ((uint32_t)(dend - dp) < lit_len) return false;
        if ((uint32_t)(send - sp) < lit_len) return false;
        memcpy(dp, sp, lit_len);
        dp += lit_len;
        sp += lit_len;

        if (dp == dend) break; /* final sequence: no match section follows */

        if (send - sp < 2) return false;
        uint32_t offset = (uint32_t)sp[0] | ((uint32_t)sp[1] << 8);
        sp += 2;
        if (offset == 0 || offset > (uint32_t)(dp - dst)) return false;

        uint32_t match_len = match_nib;
        if (match_nib == 15) {
            match_len += read_length_ext(&sp, send, &ok);
            if (!ok) return false;
        }
        match_len += EMBKZ_MINMATCH;

        if ((uint32_t)(dend - dp) < match_len) return false;
        const uint8_t *msrc = dp - offset;
        for (uint32_t i = 0; i < match_len; i++) dp[i] = msrc[i]; /* byte-wise: matches may self-overlap */
        dp += match_len;
    }

    return dp == dend;
}

int embk_compress_run_selftests(void) {
    bool ok = true;

    /* Highly compressible: a repeating pattern must actually shrink. */
    {
        uint32_t n = 4096;
        uint8_t *src = kmalloc(n);
        uint8_t *comp = kmalloc(embk_compress_bound(n));
        uint8_t *round = kmalloc(n);
        if (!src || !comp || !round) { ok = false; }
        else {
            for (uint32_t i = 0; i < n; i++) src[i] = (uint8_t)("EMBKFS-compress-test-pattern-"[i % 30]);
            uint32_t comp_len = 0;
            bool did = embk_compress(src, n, comp, embk_compress_bound(n), &comp_len);
            if (!did || comp_len >= n) {
                kprintf("EMBKFS: compress: FAIL repeating pattern did not shrink (did=%d len=%u)\n", did, (unsigned int)comp_len);
                ok = false;
            } else if (!embk_decompress(comp, comp_len, round, n) || memcmp(round, src, n) != 0) {
                kprintf("EMBKFS: compress: FAIL round-trip mismatch on repeating pattern\n");
                ok = false;
            }
        }
        kfree(src); kfree(comp); kfree(round);
    }

    /* All-zero: the degenerate maximally-compressible case. */
    {
        uint32_t n = 8192;
        uint8_t *src = kmalloc(n);
        uint8_t *comp = kmalloc(embk_compress_bound(n));
        uint8_t *round = kmalloc(n);
        if (!src || !comp || !round) { ok = false; }
        else {
            memset(src, 0, n);
            uint32_t comp_len = 0;
            bool did = embk_compress(src, n, comp, embk_compress_bound(n), &comp_len);
            if (!did || comp_len >= n / 4) {
                kprintf("EMBKFS: compress: FAIL all-zero did not compress well (did=%d len=%u)\n", did, (unsigned int)comp_len);
                ok = false;
            } else if (!embk_decompress(comp, comp_len, round, n) || memcmp(round, src, n) != 0) {
                kprintf("EMBKFS: compress: FAIL round-trip mismatch on all-zero\n");
                ok = false;
            }
        }
        kfree(src); kfree(comp); kfree(round);
    }

    /* Pseudo-random / incompressible: must be correctly reported as not
     * worth it (or, if it technically "fits", must still round-trip). */
    {
        uint32_t n = 4096;
        uint8_t *src = kmalloc(n);
        uint8_t *comp = kmalloc(embk_compress_bound(n));
        uint8_t *round = kmalloc(n);
        if (!src || !comp || !round) { ok = false; }
        else {
            uint32_t x = 0x2545F491u;
            for (uint32_t i = 0; i < n; i++) {
                x ^= x << 13; x ^= x >> 17; x ^= x << 5;
                src[i] = (uint8_t)x;
            }
            uint32_t comp_len = 0;
            bool did = embk_compress(src, n, comp, embk_compress_bound(n), &comp_len);
            if (did) {
                if (!embk_decompress(comp, comp_len, round, n) || memcmp(round, src, n) != 0) {
                    kprintf("EMBKFS: compress: FAIL round-trip mismatch on random data\n");
                    ok = false;
                }
            }
            /* did==false is the expected/common outcome here and is fine. */
        }
        kfree(src); kfree(comp); kfree(round);
    }

    /* Tiny input: must not crash, and must not claim a win it can't fit. */
    {
        uint8_t src[3] = { 1, 2, 3 };
        uint8_t comp[32];
        uint32_t comp_len = 0;
        bool did = embk_compress(src, sizeof src, comp, sizeof comp, &comp_len);
        if (did) {
            kprintf("EMBKFS: compress: FAIL claimed a win on a 3-byte input\n");
            ok = false;
        }
    }

    kprintf("EMBKFS: compress: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : -1;
}
