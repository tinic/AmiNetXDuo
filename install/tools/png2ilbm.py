#!/usr/bin/env python3
"""Convert a PNG to a 16-colour IFF ILBM, ByteRun1-compressed.

    install/tools/png2ilbm.py IN.png OUT.ilbm

Requires Pillow only when changing artwork, as compile-glowicons.py does:
packaging uses the checked-in .ilbm.  An image with more than 16 colours is
quantised to 16.

SPDX-License-Identifier: MIT
"""

import struct
import sys

from PIL import Image

PLANES = 4


def chunk(kind, payload):
    return kind + struct.pack(">I", len(payload)) + payload + b"\0" * (len(payload) & 1)


def byterun1(row):
    out = bytearray()
    i = 0
    while i < len(row):
        run = 1
        while i + run < len(row) and run < 128 and row[i + run] == row[i]:
            run += 1
        if run >= 3:
            out += bytes((257 - run, row[i]))
            i += run
            continue
        j = i
        while j < len(row) and j - i < 128 and not (
                j + 2 < len(row) and row[j] == row[j + 1] == row[j + 2]):
            j += 1
        out += bytes((j - i - 1,)) + bytes(row[i:j])
        i = j
    return bytes(out)


def main(argv):
    if len(argv) != 3:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    img = Image.open(argv[1]).convert("RGB")
    img = img.quantize(colors=1 << PLANES, dither=Image.Dither.NONE)
    w, h = img.size
    pix = img.tobytes()
    pal = (img.getpalette() or [])[:3 * (1 << PLANES)]
    pal += [0] * (3 * (1 << PLANES) - len(pal))
    rowbytes = ((w + 15) // 16) * 2

    body = bytearray()
    for y in range(h):
        line = pix[y * w:(y + 1) * w]
        for p in range(PLANES):
            plane = bytearray(rowbytes)
            for x, v in enumerate(line):
                if v >> p & 1:
                    plane[x >> 3] |= 0x80 >> (x & 7)
            body += byterun1(plane)

    # BMHD: w, h, x, y, planes, masking, compression, pad, transparent,
    # aspect x/y, page w/h.  Hires: 10:11 is the 640x256 PAL aspect.
    bmhd = struct.pack(">HHhhBBBBHBBhh", w, h, 0, 0, PLANES, 0, 1, 0, 0,
                       10, 11, w, h)
    form = b"ILBM" + chunk(b"BMHD", bmhd) + chunk(b"CMAP", bytes(pal)) \
        + chunk(b"BODY", bytes(body))
    with open(argv[2], "wb") as fh:
        fh.write(b"FORM" + struct.pack(">I", len(form)) + form)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
