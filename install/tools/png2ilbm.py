#!/usr/bin/env python3
"""Make NetSwitch's banner: a PNG to a 256-colour IFF ILBM, ByteRun1.

    install/tools/png2ilbm.py IN.png OUT.ilbm [WIDTH HEIGHT]

The image is scaled to WIDTH x HEIGHT (default 400x64, hires) and reduced to
256 colours; NetSwitch dithers it to the screen it opens on.  Requires Pillow
only when changing artwork, as compile-glowicons.py does: packaging uses the
checked-in install/banner/NetSwitch.ilbm.

SPDX-License-Identifier: MIT
"""

import struct
import sys

from PIL import Image

PLANES = 8


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
    if len(argv) not in (3, 5):
        print(__doc__.strip(), file=sys.stderr)
        return 2
    size = (int(argv[3]), int(argv[4])) if len(argv) == 5 else (400, 64)
    img = Image.open(argv[1]).convert("RGB").resize(size, Image.Resampling.LANCZOS)
    img = img.quantize(colors=1 << PLANES, method=Image.Quantize.MEDIANCUT,
                       dither=Image.Dither.NONE)
    w, h = img.size
    pix = img.tobytes()
    pal = (img.getpalette() or [])[:3 << PLANES]
    pal += [0] * ((3 << PLANES) - len(pal))
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
    # aspect x/y (10:11, PAL hires), page w/h.
    bmhd = struct.pack(">HHhhBBBBHBBhh", w, h, 0, 0, PLANES, 0, 1, 0, 0,
                       10, 11, w, h)
    form = b"ILBM" + chunk(b"BMHD", bmhd) + chunk(b"CMAP", bytes(pal)) \
        + chunk(b"BODY", bytes(body))
    with open(argv[2], "wb") as fh:
        fh.write(b"FORM" + struct.pack(">I", len(form)) + form)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
