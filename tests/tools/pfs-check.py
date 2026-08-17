#!/usr/bin/env python3
"""Parse a .pfs planar capture, check its arithmetic, and decode a frame.

    tests/tools/pfs-check.py FILE.pfs [--png OUT.png] [--frame N]

Answers the only question a capture harness cannot answer for itself: whether
the bytes are a screen.  A file of zeroes parses, has the right length and the
right frame count, and is worthless, so this also reports how many frames
differ from the one before them and how many distinct pixel values a frame
holds -- a grab that read unmapped memory or the wrong pointer fails both.

--png writes a decoded frame with no image library: one number a pixel, the
colour from the header's palette or from the pixel itself on a truecolour
capture, and a PNG assembled from zlib and struct.

Output is key=value; the exit code is the verdict.

SPDX-License-Identifier: MIT
"""

import struct
import sys
import zlib

HEADER = 16
MAGIC = b"PFS2"
FRAMEREC = 12
PTRHEAD = 16

# Header byte 9, which is rfb_geom.format: 0 is depth one-bit planes, 1 is one
# eight-bit plane of palette indices, 2 is one sixteen-bit plane of big-endian
# R5G6B5 and carries no palette at all.  The byte was documented as flags and
# only ever held 0 or 1, so both keep their old meaning and every file already
# written reads as what it is.
FMT_PLANAR = 0
FMT_CLUT8 = 1
FMT_RGB565 = 2
FMT_NAME = {FMT_PLANAR: "planar", FMT_CLUT8: "clut8", FMT_RGB565: "rgb565"}


def say(k, v):
    print("%s=%s" % (k, v))


def parse(path):
    with open(path, "rb") as fh:
        blob = fh.read()

    if len(blob) < HEADER:
        return None, "file is %d bytes, shorter than the header" % len(blob)

    magic = blob[0:4]
    width, height = struct.unpack(">HH", blob[4:8])
    depth = blob[8]
    fmt = blob[9]
    bpr, frames, pointers = struct.unpack(">HHH", blob[10:16])

    if magic != MAGIC:
        return None, "magic is %r, not %r" % (magic, MAGIC)
    if fmt not in FMT_NAME:
        return None, "format is %d, which is not one this reads" % fmt
    # Bits per pixel on the truecolour format and a plane count on the other
    # two, so what a legal depth is depends on which it is.
    if fmt == FMT_RGB565:
        if depth != 16:
            return None, "a %s capture %d deep" % (FMT_NAME[fmt], depth)
    elif not 1 <= depth <= 8:
        return None, "depth is %d" % depth
    elif fmt == FMT_CLUT8 and depth != 8:
        return None, "a %s capture %d deep" % (FMT_NAME[fmt], depth)

    # Entries the palette has, which is a property of the format: a truecolour
    # capture carries none, so its frames begin at offset 16.
    colours = (1 << depth) if fmt == FMT_PLANAR else \
              256 if fmt == FMT_CLUT8 else 0
    pal_bytes = 3 * colours
    # Planes, not the depth: both RTG formats are one plane.
    frame_bytes = (depth if fmt == FMT_PLANAR else 1) * bpr * height
    pixels = HEADER + pal_bytes + frames * frame_bytes
    want = pixels + frames * FRAMEREC

    # The pointer images are variable-length and self-sized, so the total is
    # walked rather than computed.
    at = want
    for i in range(pointers):
        if at + PTRHEAD > len(blob):
            return None, "pointer image %d runs off the end" % (i + 1)
        size = struct.unpack(">H", blob[at:at + 2])[0]
        pw, ph = struct.unpack(">HH", blob[at + 2:at + 6])
        pd = blob[at + 6]
        if not 1 <= pd <= 8:
            return None, "pointer image %d is %d planes deep" % (i + 1, pd)
        need = PTRHEAD + 3 * ((1 << pd) - 1) + pd * (((pw + 15) // 16) * 2) * ph
        if size != need:
            return None, ("pointer image %d says %d bytes, its shape needs %d"
                          % (i + 1, size, need))
        at += need
    want = at

    hdr = {
        "width": width,
        "height": height,
        "depth": depth,
        "format": FMT_NAME[fmt],
        "bytesperrow": bpr,
        "frames": frames,
        "pointers": pointers,
        "palette_bytes": pal_bytes,
        "frame_bytes": frame_bytes,
        "expect_bytes": want,
        "actual_bytes": len(blob),
    }

    if len(blob) != want:
        return hdr, "file is %d bytes, header arithmetic says %d" % (len(blob), want)
    per_byte = 8 if fmt == FMT_PLANAR else 1 if fmt == FMT_CLUT8 else 0.5
    if bpr * per_byte < width:
        return hdr, "bytesPerRow %d cannot hold %d pixels" % (bpr, width)

    hdr["_fmt"] = fmt
    hdr["_blob"] = blob
    hdr["_palette"] = blob[HEADER:HEADER + pal_bytes]
    hdr["_frames"] = [
        blob[HEADER + pal_bytes + i * frame_bytes:
             HEADER + pal_bytes + (i + 1) * frame_bytes]
        for i in range(frames)
    ]
    return hdr, None


def values(hdr, index):
    """One frame to one number per pixel.

    A palette index on the two palette formats, and the sixteen-bit colour
    itself on the truecolour one -- which is what both callers want: the
    distinct-value count is asking how much is in the picture either way, and
    the PNG writer knows which number it has.
    """
    fmt = hdr["_fmt"]
    depth = hdr["depth"]
    bpr = hdr["bytesperrow"]
    width = hdr["width"]
    height = hdr["height"]
    data = hdr["_frames"][index]

    if fmt == FMT_CLUT8:
        return bytearray(b"".join(data[y * bpr:y * bpr + width]
                                  for y in range(height)))

    if fmt == FMT_RGB565:
        out = []
        for y in range(height):
            row = data[y * bpr: y * bpr + width * 2]
            out += [(row[x * 2] << 8) | row[x * 2 + 1] for x in range(width)]
        return out

    plane_bytes = bpr * height
    out = bytearray(width * height)
    for p in range(depth):
        base = p * plane_bytes
        bit = 1 << p
        for y in range(height):
            row = data[base + y * bpr: base + (y + 1) * bpr]
            o = y * width
            for x in range(width):
                if row[x >> 3] & (0x80 >> (x & 7)):
                    out[o + x] |= bit
    return out


def rgb_of(hdr, v):
    """One pixel value to RGB8.

    Bit replication and not a shift on the truecolour format, so 0x1f comes
    out 0xff: white on the Amiga has to be white in the PNG.
    """
    if hdr["_fmt"] != FMT_RGB565:
        return hdr["_palette"][v * 3:v * 3 + 3]
    r5, g6, b5 = (v >> 11) & 0x1F, (v >> 5) & 0x3F, v & 0x1F
    return bytes(((r5 << 3) | (r5 >> 2),
                  (g6 << 2) | (g6 >> 4),
                  (b5 << 3) | (b5 >> 2)))


def write_png(path, hdr, index):
    width, height = hdr["width"], hdr["height"]
    pix = values(hdr, index)

    raw = bytearray()
    for y in range(height):
        raw.append(0)
        row = pix[y * width:(y + 1) * width]
        for v in row:
            raw += rgb_of(hdr, v)

    def chunk(tag, payload):
        return (struct.pack(">I", len(payload)) + tag + payload +
                struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")

    with open(path, "wb") as fh:
        fh.write(png)


def main(argv):
    if len(argv) < 2:
        print(__doc__.strip(), file=sys.stderr)
        return 2

    path = argv[1]
    png = None
    frame = 0
    i = 2
    while i < len(argv):
        if argv[i] == "--png":
            png = argv[i + 1]
            i += 2
        elif argv[i] == "--frame":
            frame = int(argv[i + 1])
            i += 2
        else:
            print("unknown argument: %s" % argv[i], file=sys.stderr)
            return 2

    hdr, err = parse(path)
    if hdr is not None:
        for key in ("width", "height", "depth", "format", "bytesperrow",
                    "frames", "pointers", "palette_bytes", "frame_bytes",
                    "expect_bytes", "actual_bytes"):
            say(key, hdr[key])
    if err is not None:
        say("error", err)
        say("RESULT", "FAIL")
        return 1

    frames = hdr["_frames"]
    changed = sum(1 for i in range(1, len(frames)) if frames[i] != frames[i - 1])
    say("frames_changed", changed)
    say("frames_identical_to_previous", max(len(frames) - 1, 0) - changed)

    zero = sum(1 for f in frames if not any(f))
    say("frames_all_zero", zero)

    # The palette a screen really has: 3.1's Workbench is grey/black/white/blue
    # and a grab that missed the ColorMap has nothing but zeroes here.  A
    # truecolour capture has no palette, so this is 0 and says nothing; the
    # format key above is what tells the two apart.
    say("palette_nonzero", sum(1 for b in hdr["_palette"] if b))

    if frames:
        pix = values(hdr, min(frame, len(frames) - 1))
        say("frame%d_distinct_values" % frame, len(set(pix)))
        say("frame%d_set_pixels" % frame, sum(1 for v in pix if v))

    if png is not None:
        write_png(png, hdr, min(frame, len(frames) - 1))
        say("png", png)

    ok = frames and zero == 0
    say("RESULT", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
