"""
crc32c.py — CRC32C (Castagnoli) checksum, written by hand so its result is
GUARANTEED to match the kernel's hardware CRC32C instruction. This is the same
checksum EMBKFS uses for every block pointer, every node, and every extent.

Why not Python's binascii.crc32? That computes CRC-32 with the *ISO/zlib*
polynomial (0xEDB88320 reflected). EMBKFS specs CRC32C, which uses the
*Castagnoli* polynomial (0x1EDC6F41, i.e. 0x82F63B78 reflected). The two give
different values for the same input. The kernel will use CRC32C (the x86 `crc32`
instruction and ARM's CRC32C both implement Castagnoli), so the formatter must
too, or the kernel will reject every checksum.

We use the standard table-driven, reflected (LSB-first) form, which matches the
hardware instruction's bit ordering.
"""

# Castagnoli polynomial, reflected form.
_POLY = 0x82F63B78

# Precompute the 256-entry lookup table once at import.
_TABLE = []
for _n in range(256):
    _c = _n
    for _ in range(8):
        # If the low bit is set, shift right and XOR the reflected polynomial;
        # otherwise just shift right. This is the standard reflected CRC step.
        _c = (_c >> 1) ^ _POLY if (_c & 1) else (_c >> 1)
    _TABLE.append(_c)


def crc32c(data: bytes, crc: int = 0) -> int:
    """
    Compute CRC32C over `data`. `crc` allows continuing a previous computation
    (seed). Returns a 32-bit unsigned integer.

    The algorithm:
      - start with crc inverted (all ones) — the conventional initial value
      - for each byte, index the table by (low byte of crc) XOR (data byte),
        then XOR with crc shifted right 8
      - invert at the end
    This matches the hardware CRC32C result exactly.
    """
    crc = (crc ^ 0xFFFFFFFF) & 0xFFFFFFFF
    for b in data:
        crc = _TABLE[(crc ^ b) & 0xFF] ^ (crc >> 8)
    return (crc ^ 0xFFFFFFFF) & 0xFFFFFFFF


if __name__ == "__main__":
    # The canonical CRC32C test vector: the ASCII string "123456789" must
    # produce 0xE3069283. This is the standard check value for CRC32C and is
    # what hardware implementations are validated against. If this passes, our
    # CRC32C matches the kernel's.
    test = b"123456789"
    got = crc32c(test)
    expected = 0xE3069283
    print(f"CRC32C('123456789') = 0x{got:08X}  (expected 0x{expected:08X})")
    assert got == expected, "CRC32C MISMATCH — would not match the kernel!"
    print("CRC32C self-test PASSED — matches the standard Castagnoli check value.")

    # A couple more sanity checks.
    print(f"CRC32C(b'')        = 0x{crc32c(b''):08X}  (empty input)")
    print(f"CRC32C(b'a')       = 0x{crc32c(b'a'):08X}")
