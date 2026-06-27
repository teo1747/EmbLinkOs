#include "crc32c.h"

/* Castagnoli polynomial in reflected form (low bit = highest-order term).
 * The reflected form is what makes the table loop shift RIGHT and test the
 * LOW bit — the LSB-first ordering that matches the hardware crc32 op. */
#define EMBK_CRC32C_POLY 0x82F63B78u

/* Built once by embk_crc32c_init(). Each entry crc32c_table[b] is the CRC
 * contribution of the single byte value b, so the main loop folds in a whole
 * byte per iteration instead of looping over 8 bits. */
static uint32_t crc32c_table[256];

void embk_crc32c_init(void)
{
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        /* Standard reflected build: eight times — if the low bit is set,
         * shift right and XOR the polynomial; otherwise just shift right. */
        for (int k = 0; k < 8; k++)
            c = (c & 1u) ? (c >> 1) ^ EMBK_CRC32C_POLY : (c >> 1);
        crc32c_table[n] = c;
    }
}

uint32_t embk_crc32c(const void *data, size_t len, uint32_t seed)
{
    const uint8_t *p = (const uint8_t *)data;

    /* Init: invert the seed. With seed == 0 this is the conventional all-ones
     * 0xFFFFFFFF start. Inverting again at the end is what lets a returned
     * value be fed back in as `seed` to continue the CRC. */
    uint32_t crc = seed ^ 0xFFFFFFFFu;

    for (size_t i = 0; i < len; i++) {
        /* Index the table by (low byte of crc) XOR (next data byte), then fold
         * the register down with a logical (zero-fill) >> 8. uint32_t is what
         * guarantees both the mod-2^32 wrap and the zero-fill shift. */
        crc = crc32c_table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    }

    return crc ^ 0xFFFFFFFFu;
}