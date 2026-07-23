#!/usr/bin/env python3
"""mkembx.py -- bake a static ELF into a byte-exact EMBX application image.

The seed of the EMBX producer family (embas/embld/embpack in the spec's tool
list). It does exactly one job: take a FULLY LINKED static ELF -- the shape
EMBX_TYPE_APP requires (no relocations) -- and repackage its PT_LOAD segments
into an .embx, adding the one thing ELF cannot carry: a declared capability
table. It does not compile, link, or relocate; those are the toolchain's, and
an APP is already linked by the time it reaches here.

    mkembx.py <in.elf> <out.embx> [--cap NAME ...]

Every field and offset matches EMBX_Specification_v2 §3; the checksum ordering
is §3.4 exactly, so two identical inputs produce byte-identical output (§2.5).

This is a PRODUCER, deliberately trusting its ELF input; the guards live in the
loader (§8), which is where an image of unknown provenance is actually checked.
"""
import sys, os, struct, hashlib

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "embkfs_mkfs"))
from crc32c import crc32c

EMBX_MAGIC = bytes([0x7F, 0x45, 0x4D, 0x42, 0x58, 0x0D, 0x0A, 0x1A])  # .EMBX\r\n\x1A
EMBX_TYPE_APP = 1
EMBX_MACHINE_X86_64 = 1
EMBX_ABI_VERSION = 1
EMBX_SEG_LOAD = 1
HDR_SIZE, SEG_SIZE, CAP_SIZE = 128, 64, 16

# Capability IDs -- must match kernel capabilities.h / EMBX §5.6.
CAPS = {"FILESYSTEM":1,"NETWORK":2,"GPU":3,"AUDIO":4,"CAMERA":5,
        "USB":6,"SERIAL":7,"RAWDISK":8,"KERNEL_EXT":9}

PT_LOAD = 1
PF_X, PF_W, PF_R = 1, 2, 4
SEG_R, SEG_W, SEG_X = 1, 2, 4


def read_elf_loads(elf):
    if elf[:4] != b"\x7fELF" or elf[4] != 2:        # ELFCLASS64
        sys.exit("mkembx: input is not a 64-bit ELF")
    e_entry   = struct.unpack_from("<Q", elf, 0x18)[0]
    e_phoff   = struct.unpack_from("<Q", elf, 0x20)[0]
    e_phentsz = struct.unpack_from("<H", elf, 0x36)[0]
    e_phnum   = struct.unpack_from("<H", elf, 0x38)[0]
    loads = []
    for i in range(e_phnum):
        base = e_phoff + i * e_phentsz
        (p_type, p_flags, p_offset, p_vaddr, _paddr,
         p_filesz, p_memsz, p_align) = struct.unpack_from("<IIQQQQQQ", elf, base)
        if p_type != PT_LOAD:
            continue
        flags = 0
        if p_flags & PF_R: flags |= SEG_R
        if p_flags & PF_W: flags |= SEG_W
        if p_flags & PF_X: flags |= SEG_X
        loads.append(dict(vaddr=p_vaddr, off=p_offset, filesz=p_filesz,
                          memsz=p_memsz, align=max(p_align, 4096), flags=flags,
                          data=elf[p_offset:p_offset + p_filesz]))
    if not loads:
        sys.exit("mkembx: no PT_LOAD segments")
    loads.sort(key=lambda s: s["vaddr"])            # §8: LOAD sorted by vaddr
    return e_entry, loads


def congruent_offset(cur, vaddr, align):
    """Smallest file offset >= cur with (offset % align) == (vaddr % align),
    the §8 congruence rule that keeps a future demand-pager able to map file
    pages directly."""
    return cur + ((vaddr - cur) % align)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    inp, outp = sys.argv[1], sys.argv[2]
    caps = []
    i = 3
    while i < len(sys.argv):
        if sys.argv[i] == "--cap":
            name = sys.argv[i + 1].upper()
            if name not in CAPS: sys.exit(f"mkembx: unknown capability {name}")
            caps.append(CAPS[name]); i += 2
        else:
            sys.exit(f"mkembx: unexpected arg {sys.argv[i]}")
    caps = sorted(set(caps))                        # §5.5 sorted + unique

    elf = open(inp, "rb").read()
    entry, loads = read_elf_loads(elf)

    seg_tab_off = HDR_SIZE
    cap_tab_off = seg_tab_off + len(loads) * SEG_SIZE
    payload_start = cap_tab_off + len(caps) * CAP_SIZE

    # Place each segment's payload honoring the congruence rule, and record the
    # per-segment checksum over the bytes that actually land.
    cur = payload_start
    for s in loads:
        s["file_offset"] = congruent_offset(cur, s["vaddr"], s["align"])
        s["checksum"] = crc32c(s["data"]) if s["filesz"] else 0
        cur = s["file_offset"] + s["filesz"]
    image_size = cur

    img = bytearray(image_size)

    # segment table
    for idx, s in enumerate(loads):
        struct.pack_into("<IIQQQQQII", img, seg_tab_off + idx * SEG_SIZE,
                         EMBX_SEG_LOAD, s["flags"], s["vaddr"], s["file_offset"],
                         s["filesz"], s["memsz"], s["align"], s["checksum"], 0)
        # paddr (last 8 bytes) stays 0 for a non-BOOT kind
        struct.pack_into("<Q", img, seg_tab_off + idx * SEG_SIZE + 0x38, 0)

    # capability table (cap_id, cap_flags=0, reserved0=0)
    for idx, cid in enumerate(caps):
        struct.pack_into("<IIQ", img, cap_tab_off + idx * CAP_SIZE, cid, 0, 0)

    # segment payloads
    for s in loads:
        img[s["file_offset"]:s["file_offset"] + s["filesz"]] = s["data"]

    # header -- build_id and header_checksum zero for now (§3.4 order)
    struct.pack_into("<8sHHIHHIQQQQIHHIHHII", img, 0,
        EMBX_MAGIC, 1, 0, HDR_SIZE, EMBX_TYPE_APP, EMBX_MACHINE_X86_64,
        EMBX_ABI_VERSION, 0, 0, 0, entry,
        seg_tab_off, len(loads), SEG_SIZE,
        cap_tab_off if caps else 0, len(caps), CAP_SIZE,
        0, 0)                                       # grantor(0x48)=0, reserved0(0x4C)=0
    struct.pack_into("<Q", img, 0x70, image_size)   # image_size
    struct.pack_into("<I", img, 0x78, 0)            # reserved1

    # (3) build_id = SHA-256 over the whole file with build_id + hdr_csum zeroed
    build_id = hashlib.sha256(bytes(img)).digest()
    img[0x50:0x70] = build_id

    # (4) header_checksum = CRC32C over bytes 0x00..0x7B (it is the last field)
    struct.pack_into("<I", img, 0x7C, crc32c(bytes(img[:0x7C])))

    open(outp, "wb").write(img)
    print(f"mkembx: {outp}  ({image_size} bytes, {len(loads)} segment(s), "
          f"entry 0x{entry:x}, caps {caps or 'none'})")


if __name__ == "__main__":
    main()
