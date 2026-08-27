#!/usr/bin/env python3
"""Check a console PNG against the picture tests/perf/chipscreen.c drew.

    tests/tools/chipscreen-check.py FILE.png --pattern ham6|ham8|ehb|planar
                                    --size WxH [--depth N] [--tolerance N]

WHAT IT IS FOR.  The console harness could say a chipset arm passed on the
strength of a geom word and an exit code, and both of those are right on a
picture that is one column out.  That is the shape of the defect 1ec557b9
fixed: BMA_WIDTH answers for the ALLOCATION, a planar screen rounds that up
to 16, and a 312-wide screen sits in a 320-wide bitmap whose last 8 columns
belong to nobody.  Serving them costs one wrong column of pixels and a wrong
width in the geom word, and nothing in the lab could see either, because
every resolution tested was a multiple of 16.

So this reads the decoded frame back and compares every pixel with the one
chipscreen drew.  The size is checked first and separately: a picture 320
wide when the screen is 312 is the padding defect itself, and it is worth a
different sentence from "the colours are wrong".

THE REFERENCE.  Recomputed from the drawing rules in tests/perf/chipscreen.c
-- base_rgb(), ham_row(), ehb_row(), planar_row() -- and from the chipset's
own rule for turning an index into a colour.  Both files must change
together; the pattern name in the guest's report is what ties one run to one
rule here.

TOLERANCE.  Every component chipscreen loads is a multiple of 17, so a step
of the ramp is 17 and an exact comparison is possible.  The default is 8
because extra half-brite has two defensible halvings -- the eight-bit
component shifted right, which is what the receiver does, and the four-bit
colour register shifted right, which is what the hardware does -- and they
differ by up to 8.  A column out of place moves more than that.

Output is key=value; the exit code is the verdict: 0 pass, 1 the picture is
not the one that was drawn, 2 the file could not be read at all.

SPDX-License-Identifier: MIT
"""

import struct
import sys
import zlib

PATTERNS = ("planar", "ham6", "ham8", "ehb")


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

    Written out by hand for the reason tests/tools/rtgbars-check.py has one:
    a build host is not required to have an image library, and a missing
    dependency must not turn a wrong picture into an infrastructure result."""
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


# ------------------------------------------------- what the guest drew --


def base_rgb(i):
    """chipscreen.c base_rgb().  Entry 0 must be black: a HAM row starts
    from it."""
    if i == 0:
        return (0, 0, 0)
    if i == 1:
        return (255, 255, 255)
    return (17 * (i & 15),
            17 * (15 - (i & 15)),
            17 * (((i * 7) + (i >> 4)) & 15))


def colours_for(kind, depth):
    """chipscreen.c colours_for().  HAM6 reads sixteen entries and HAM8
    sixty-four whatever their depth says; EHB reads thirty-two."""
    if kind == "ham6":
        return 16
    if kind == "ham8":
        return 64
    if kind == "ehb":
        return 32
    return 1 << depth


def row_values(kind, w, h, y, depth):
    """chipscreen.c ham_row(), ehb_row() and planar_row(), one row."""
    band = (y * 4) // h

    if kind in ("ham6", "ham8"):
        shift = 4 if kind == "ham6" else 6
        n = 1 << shift
        lead = {0: 0, 1: 2 << shift, 2: 3 << shift}.get(band, 1 << shift)
        return [lead | ((x * n) // w) for x in range(w)]

    if kind == "ehb":
        out = []
        for x in range(w):
            k32 = (x * 32) // w
            k64 = (x * 64) // w
            if band == 0:
                out.append(k32)
            elif band == 1:
                out.append(32 + k32)
            elif band == 2:
                out.append(k64)
            else:
                out.append((y + k64) & 63)
        return out

    n = 1 << depth
    return [((y + ((x * n) // w)) & (n - 1)) if band == 3 else ((x * n) // w)
            for x in range(w)]


def decode_row(kind, values, pal):
    """Index to colour, the chipset's own rule.

    HAM is a chain and not a table: a pixel either names a base colour or
    replaces one component of the pixel to its LEFT, and a row starts from
    base colour 0, which is what the hardware does at the start of a
    scanline.  The control codes are 0 base, 1 blue, 2 red, 3 green.

    EHB index 32 + k is entry k with every component halved.  The eight-bit
    component is what is halved here, which is the receiver's rule; the
    hardware halves the four-bit register and lands up to 8 lower.  --tolerance
    covers the difference.
    """
    out = []

    if kind in ("ham6", "ham8"):
        ham6 = kind == "ham6"
        shift, mask = (4, 0x0F) if ham6 else (6, 0x3F)
        r, g, b = pal[0]
        for v in values:
            ctl = v >> shift
            data = v & mask
            if ctl == 0:
                r, g, b = pal[data]
            elif ctl == 1:
                b = data * 17 if ham6 else ((data << 2) | (b & 3))
            elif ctl == 2:
                r = data * 17 if ham6 else ((data << 2) | (r & 3))
            else:
                g = data * 17 if ham6 else ((data << 2) | (g & 3))
            out.append((r, g, b))
        return out

    if kind == "ehb":
        for v in values:
            if v < 32:
                out.append(pal[v])
            else:
                c = pal[v - 32]
                out.append((c[0] >> 1, c[1] >> 1, c[2] >> 1))
        return out

    for v in values:
        out.append(pal[v])
    return out


def main(argv):
    path = None
    kind = None
    want_w = want_h = 0
    depth = 0
    tol = 8

    i = 1
    while i < len(argv):
        if argv[i] == "--pattern":
            kind = argv[i + 1]; i += 2
        elif argv[i] == "--size":
            try:
                want_w, want_h = (int(v) for v in argv[i + 1].split("x"))
            except Exception:                       # noqa: BLE001
                say("error", "--size takes WxH, not %s" % argv[i + 1])
                return 2
            i += 2
        elif argv[i] == "--depth":
            depth = int(argv[i + 1]); i += 2
        elif argv[i] == "--tolerance":
            tol = int(argv[i + 1]); i += 2
        elif path is None:
            path = argv[i]; i += 1
        else:
            say("error", "unexpected argument %s" % argv[i])
            return 2

    if path is None or kind is None or want_w == 0:
        say("error", "usage: chipscreen-check.py FILE.png --pattern P "
                     "--size WxH [--depth N]")
        return 2
    if kind not in PATTERNS:
        say("error", "--pattern takes one of %s, not %s"
                     % (" ".join(PATTERNS), kind))
        return 2
    if kind == "planar" and depth == 0:
        say("error", "--pattern planar needs --depth")
        return 2
    if depth == 0:
        depth = 8 if kind == "ham8" else 6

    try:
        w, h, rows = read_png(path)
    except Exception as exc:                        # noqa: BLE001
        say("error", "%s: %s" % (path, exc))
        say("RESULT", "INFRA")
        return 2

    say("png", path)
    say("pattern", kind)
    say("png_size", "%dx%d" % (w, h))
    say("screen_size", "%dx%d" % (want_w, want_h))
    say("tolerance", tol)

    # FIRST AND SEPARATELY.  A picture wider than the screen is the padding
    # defect and not a colour error, and it has its own place to look.
    if w != want_w or h != want_h:
        say("size_ok", "no")
        say("error", "the frame is %dx%d and the screen is %dx%d: the console "
                     "is serving the bitmap's allocation, not the screen"
                     % (w, h, want_w, want_h))
        say("hint", "the DWidth clamp in fb_geometry_of(), src/tools/httpfb.c, "
                    "and http_rtg_describe(), src/tools/httprtg.c")
        say("RESULT", "FAIL")
        return 1
    say("size_ok", "yes")

    pal = [base_rgb(i) for i in range(colours_for(kind, depth))]

    bad = 0
    worst = 0
    first = None
    for y in range(h):
        want = decode_row(kind, row_values(kind, w, h, y, depth), pal)
        row = rows[y]
        for x in range(w):
            got = (row[x * 3], row[x * 3 + 1], row[x * 3 + 2])
            off = max(abs(got[c] - want[x][c]) for c in range(3))
            if off > tol:
                bad += 1
                if off > worst:
                    worst = off
                if first is None:
                    first = (x, y, got, want[x])

    say("pixels", w * h)
    say("mismatched", bad)
    say("worst_channel_error", worst)

    if first is not None:
        x, y, got, wanted = first
        say("first_wrong", "at %d,%d got %d,%d,%d want %d,%d,%d"
                           % (x, y, got[0], got[1], got[2],
                              wanted[0], wanted[1], wanted[2]))

    if bad == 0:
        say("RESULT", "PASS")
        return 0
    say("RESULT", "FAIL")
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
