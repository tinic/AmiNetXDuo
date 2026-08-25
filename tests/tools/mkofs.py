#!/usr/bin/env python3
# mkofs, an empty 880 KB OFS floppy image.
# SPDX-License-Identifier: MIT

import struct
import sys

BSIZE = 512
BLOCKS = 1760                                   # 880 KB, double density
ROOT = 880
BITMAP = 881

T_HEADER = 2
ST_ROOT = 1


def checksum(block, off):
    """Make the sum of the block's longs zero, with the sum field at `off`."""
    b = bytearray(block)
    b[off:off + 4] = b"\0\0\0\0"
    total = 0
    for i in range(0, BSIZE, 4):
        total = (total + struct.unpack_from(">I", b, i)[0]) & 0xFFFFFFFF
    b[off:off + 4] = struct.pack(">I", (-total) & 0xFFFFFFFF)
    return bytes(b)


def root_block(name):
    b = bytearray(BSIZE)
    struct.pack_into(">I", b, 0, T_HEADER)
    struct.pack_into(">I", b, 12, 0x48)             # ht_size
    struct.pack_into(">i", b, BSIZE - 200, -1)      # bm_flag: bitmap is valid
    struct.pack_into(">I", b, BSIZE - 196, BITMAP)  # bm_pages[0]
    n = name.encode("latin-1")[:30]
    b[BSIZE - 80] = len(n)                          # the volume name, a BSTR
    b[BSIZE - 79:BSIZE - 79 + len(n)] = n
    struct.pack_into(">I", b, BSIZE - 4, ST_ROOT)
    return checksum(bytes(b), 20)


def bitmap_block(used):
    """One bit per block from block 2 up, set when the block is FREE."""
    b = bytearray(BSIZE)
    bits = bytearray(BSIZE - 4)
    for i in range((BLOCKS - 2 + 7) // 8):
        bits[i] = 0xFF
    for blk in used:
        idx = blk - 2
        word = struct.unpack_from(">I", bits, (idx // 32) * 4)[0]
        struct.pack_into(">I", bits, (idx // 32) * 4, word & ~(1 << (idx % 32)))
    b[4:] = bits
    return checksum(bytes(b), 0)


def main():
    if len(sys.argv) < 2:
        print("usage: mkofs.py OUT.adf [VOLUME]", file=sys.stderr)
        return 2

    out = sys.argv[1]
    name = sys.argv[2] if len(sys.argv) > 2 else "Drill"

    img = bytearray(BSIZE * BLOCKS)
    img[0:4] = b"DOS\0"                             # OFS.  DOS\1 would be FFS
    struct.pack_into(">I", img, 8, ROOT)

    img[ROOT * BSIZE:(ROOT + 1) * BSIZE] = root_block(name)
    img[BITMAP * BSIZE:(BITMAP + 1) * BSIZE] = bitmap_block([ROOT, BITMAP])

    with open(out, "wb") as f:
        f.write(img)

    print("%s: %d bytes, OFS, volume %s" % (out, len(img), name))
    return 0


if __name__ == "__main__":
    sys.exit(main())
