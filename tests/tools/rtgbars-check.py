#!/usr/bin/env python3
"""Check a console PNG against the colour bars tests/perf/rtgbars.c drew.

    tests/tools/rtgbars-check.py FILE.png [--tolerance N] [--bars N]

WHAT IT IS FOR.  A truecolour console session had only ever served Workbench,
and a frame of Workbench decodes, has many colours in it and changes over time
whether or not the readback exchanged red and blue on the way.  Every assertion
the harness had passes on the transposed picture.  So the guest draws eight
bands of known colour instead -- black, red, green, blue, yellow, magenta,
cyan, white -- and this reads them back off the decoded frame.

It names a transposition rather than only failing on one.  Red at one eighth
across and blue at three eighths is the pair that tells them apart, and a
readback with the two channels crossed reports every band as the colour with R
and B exchanged, which is a distinct answer from "the picture is wrong".

TOLERANCE.  Everything drawn is a full-scale component or none, and full scale
survives R5G6B5 unchanged in either direction, so the expected values are exact
at 15, 16, 24 and 32 bits alike and the default tolerance is small.

Output is key=value; the exit code is the verdict: 0 pass, 1 the picture is not
the bars, 2 the file could not be read at all.

SPDX-License-Identifier: MIT
"""

import struct
import sys
import zlib

# The same eight, in the same order, as tests/perf/rtgbars.c.  Both files print
# their list -- rtgbars in its `bars=` line and this one in its output -- so a
# change to one that misses the other is visible rather than silent.
BARS = [
    ("black",   (0x00, 0x00, 0x00)),
    ("red",     (0xFF, 0x00, 0x00)),
    ("green",   (0x00, 0xFF, 0x00)),
    ("blue",    (0x00, 0x00, 0xFF)),
    ("yellow",  (0xFF, 0xFF, 0x00)),
    ("magenta", (0xFF, 0x00, 0xFF)),
    ("cyan",    (0x00, 0xFF, 0xFF)),
    ("white",   (0xFF, 0xFF, 0xFF)),
]


def say(k, v):
    print("%s=%s" % (k, v))


def paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def read_png(path):
    """(width, height, rows) with rows as bytes of 8-bit RGB triples.

    Written out by hand because a build host is not required to have an image
    library, and the one thing a missing dependency must not do here is turn a
    wrong picture into an infrastructure result."""
    with open(path, "rb") as fh:
        blob = fh.read()

    if blob[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG: magic is %r" % blob[:8])

    at = 8
    w = h = 0
    depth = colour = 0
    idat = bytearray()
    while at + 8 <= len(blob):
        (n,) = struct.unpack(">I", blob[at:at + 4])
        tag = blob[at + 4:at + 8]
        payload = blob[at + 8:at + 8 + n]
        at += 12 + n
        if tag == b"IHDR":
            w, h, depth, colour = struct.unpack(">IIBB", payload[:10])
        elif tag == b"IDAT":
            idat += payload
        elif tag == b"IEND":
            break

    if depth != 8 or colour != 2:
        raise ValueError("bit depth %d colour type %d is not 8-bit RGB"
                         % (depth, colour))
    if w == 0 or h == 0:
        raise ValueError("the header says %dx%d" % (w, h))

    raw = zlib.decompress(bytes(idat))
    stride = w * 3
    if len(raw) != (stride + 1) * h:
        raise ValueError("%d bytes of pixels, not %d"
                         % (len(raw), (stride + 1) * h))

    rows = []
    prev = bytearray(stride)
    at = 0
    for _ in range(h):
        ft = raw[at]
        line = bytearray(raw[at + 1:at + 1 + stride])
        at += 1 + stride
        if ft == 1:
            for i in range(3, stride):
                line[i] = (line[i] + line[i - 3]) & 0xFF
        elif ft == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif ft == 3:
            for i in range(stride):
                left = line[i - 3] if i >= 3 else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 0xFF
        elif ft == 4:
            for i in range(stride):
                left = line[i - 3] if i >= 3 else 0
                up = prev[i]
                ul = prev[i - 3] if i >= 3 else 0
                line[i] = (line[i] + paeth(left, up, ul)) & 0xFF
        elif ft != 0:
            raise ValueError("filter type %d on a row" % ft)
        rows.append(bytes(line))
        prev = line
    return w, h, rows


def sample(rows, w, h, x0, x1):
    """The commonest colour in the middle of a band.

    A mode and not a mean: a mean of a band that is half right and half wrong
    is a colour that was never on the screen, and would be reported as a small
    error in every channel rather than as the two halves it is."""
    lo = x0 + (x1 - x0) // 4
    hi = x1 - (x1 - x0) // 4
    # Down to 60% of the height and no further: rtgbars blinks a square in the
    # bottom-left corner, and a sample that caught it would report the black
    # band as white every other frame.
    ys = range(h // 10, max(h // 10 + 1, (h * 6) // 10), max(1, h // 40))
    xs = range(lo, max(lo + 1, hi), max(1, (hi - lo) // 16))
    seen = {}
    for y in ys:
        row = rows[y]
        for x in xs:
            px = (row[x * 3], row[x * 3 + 1], row[x * 3 + 2])
            seen[px] = seen.get(px, 0) + 1
    best = max(seen.items(), key=lambda kv: kv[1])
    return best[0], best[1], sum(seen.values())


def near(a, b, tol):
    return all(abs(a[i] - b[i]) <= tol for i in range(3))


def main(argv):
    path = None
    tol = 12
    nbars = len(BARS)
    i = 1
    while i < len(argv):
        if argv[i] == "--tolerance":
            tol = int(argv[i + 1]); i += 2
        elif argv[i] == "--bars":
            nbars = int(argv[i + 1]); i += 2
        elif path is None:
            path = argv[i]; i += 1
        else:
            say("error", "unexpected argument %s" % argv[i])
            return 2
    if path is None:
        say("error", "no PNG named")
        return 2

    try:
        w, h, rows = read_png(path)
    except Exception as exc:                        # noqa: BLE001
        say("error", "%s: %s" % (path, exc))
        say("RESULT", "INFRA")
        return 2

    say("png", path)
    say("size", "%dx%d" % (w, h))
    say("tolerance", tol)

    want = BARS[:nbars]
    hits = 0
    swaps = 0
    for i, (name, rgb) in enumerate(want):
        x0 = (w * i) // nbars
        x1 = (w * (i + 1)) // nbars - 1 if i + 1 < nbars else w - 1
        got, votes, total = sample(rows, w, h, x0, x1)
        crossed = (rgb[2], rgb[1], rgb[0])
        ok = near(got, rgb, tol)
        rb = (not ok) and near(got, crossed, tol)
        if ok:
            hits += 1
        if rb:
            swaps += 1
        say("band%d" % i,
            "%s want %d,%d,%d got %d,%d,%d %s (%d/%d of the samples)"
            % (name, rgb[0], rgb[1], rgb[2], got[0], got[1], got[2],
               "ok" if ok else ("red and blue exchanged" if rb else "wrong"),
               votes, total))

    say("bands_ok", "%d/%d" % (hits, len(want)))

    # Named, not merely failed.  A picture whose red and blue are crossed is a
    # specific defect with a specific place to look -- the two RGBFTYPE codes
    # in src/tools/httprtg.c -- and it looks deliberate rather than broken, so
    # it is worth saying out loud rather than leaving as "the bands are wrong".
    if swaps > 0:
        say("red_blue_exchanged", "%d of %d bands" % (swaps, len(want)))
        say("hint", "the readback's channel order is reversed; see the "
                    "RGBFTYPE and PIXFMT tables in src/tools/httprtg.c")

    if hits == len(want):
        say("RESULT", "PASS")
        return 0
    say("RESULT", "FAIL")
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
