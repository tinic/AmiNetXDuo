#!/usr/bin/env python3
"""Parse a .pfs planar capture, check its arithmetic, and decode a frame.

    tests/tools/pfs-check.py FILE.pfs [--png OUT.png] [--frame N]

Answers the only question a capture harness cannot answer for itself: whether
the bytes are a screen.  A file of zeroes parses, has the right length and the
right frame count, and is worthless, so this also reports how many frames
differ from the one before them and how many distinct pixel values a frame
holds -- a grab that read unmapped memory or the wrong pointer fails both.

--png writes a decoded frame with no image library: planar to chunky, the
palette out of the header, and a PNG assembled from zlib and struct.

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
    flags = blob[9]
    bpr, frames, pointers = struct.unpack(">HHH", blob[10:16])

    if magic != MAGIC:
        return None, "magic is %r, not %r" % (magic, MAGIC)
    if not 1 <= depth <= 8:
        return None, "depth is %d" % depth

    pal_bytes = 3 * (1 << depth)
    frame_bytes = depth * bpr * height
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
        "flags": flags,
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
    if bpr * 8 < width:
        return hdr, "bytesPerRow %d cannot hold %d pixels" % (bpr, width)

    hdr["_blob"] = blob
    hdr["_palette"] = blob[HEADER:HEADER + pal_bytes]
    hdr["_frames"] = [
        blob[HEADER + pal_bytes + i * frame_bytes:
             HEADER + pal_bytes + (i + 1) * frame_bytes]
        for i in range(frames)
    ]
    return hdr, None


def chunky(hdr, index):
    """One frame, planar and plane-major, to one byte per pixel."""
    depth = hdr["depth"]
    bpr = hdr["bytesperrow"]
    width = hdr["width"]
    height = hdr["height"]
    data = hdr["_frames"][index]
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


def write_png(path, hdr, index):
    width, height = hdr["width"], hdr["height"]
    pal = hdr["_palette"]
    pix = chunky(hdr, index)

    raw = bytearray()
    for y in range(height):
        raw.append(0)
        row = pix[y * width:(y + 1) * width]
        for v in row:
            raw += pal[v * 3:v * 3 + 3]

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
        for key in ("width", "height", "depth", "flags", "bytesperrow",
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
    # and a grab that missed the ColorMap has 3 * (1 << depth) zeroes here.
    say("palette_nonzero", sum(1 for b in hdr["_palette"] if b))

    if frames:
        pix = chunky(hdr, min(frame, len(frames) - 1))
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
