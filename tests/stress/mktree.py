#!/usr/bin/env python3
"""
The directory tree worker 4 copies to the share.

    tests/stress/mktree.py OUTDIR [ADF ...]

With ADFs, their contents are extracted with the directory structure intact --
a real Amiga system installation, which is what the test this harness copies
uses ("copy sys: temp:testsys all quiet clone").  Without them, a tree of the
same shape is synthesised: the same nesting, the same wide file-size
distribution, deterministic from a fixed seed so the host can rebuild it and
compare.

Neither carries Amiga protection bits or comments -- a host filesystem has no
place to put them and UAE's directory filesystem only writes its .uaem sidecar
when the guest sets something.  FitzStress stamps both onto this tree before
the workers start, so what Fitz carries over the wire and comparetree checks is
present either way.

SPDX-License-Identifier: MIT
"""

import os
import struct
import sys

BS = 512


def u32(b, o):
    return struct.unpack_from(">I", b, o)[0]


def i32(b, o):
    return struct.unpack_from(">i", b, o)[0]


class ADF:
    """Minimal OFS/FFS reader -- enough to walk and read a Workbench floppy."""

    def __init__(self, path):
        self.d = open(path, "rb").read()
        self.ffs = bool(self.d[3] & 1)
        self.root = 880

    def blk(self, n):
        return self.d[n * BS:(n + 1) * BS]

    def name(self, b):
        n = b[BS - 80]
        return b[BS - 79:BS - 79 + n].decode("latin-1")

    def entries(self, hdr):
        b = self.blk(hdr)
        ht = u32(b, 12) or 72
        out = []
        for i in range(ht):
            p = u32(b, 24 + 4 * i)
            while p:
                eb = self.blk(p)
                out.append((self.name(eb), p, i32(eb, BS - 4)))
                p = u32(eb, BS - 16)
        return out

    def read_file(self, hdr):
        b = self.blk(hdr)
        size = u32(b, BS - 188)
        data = bytearray()
        cur = hdr
        while cur:
            cb = self.blk(cur)
            n = u32(cb, 8)
            for i in range(n):
                dp = u32(cb, 24 + 4 * (71 - i))
                if not dp:
                    continue
                db = self.blk(dp)
                data += db if self.ffs else db[24:24 + u32(db, 12)]
            cur = u32(cb, BS - 8)
        return bytes(data[:size])

    def extract(self, dest, hdr=None, rel=""):
        """Unlike ~/adfx.py this keeps the path: a flattened tree is not one."""
        if hdr is None:
            hdr = self.root
        files = 0
        for nm, blk, st in self.entries(hdr):
            # A name AmigaDOS accepts can still be awkward on the host side.
            safe = nm.replace("/", "_")
            sub = os.path.join(rel, safe)
            if st == 2:
                os.makedirs(os.path.join(dest, sub), exist_ok=True)
                files += self.extract(dest, blk, sub)
            else:
                path = os.path.join(dest, sub)
                os.makedirs(os.path.dirname(path), exist_ok=True)
                with open(path, "wb") as fh:
                    fh.write(self.read_file(blk))
                files += 1
        return files


# The shape of a 3.1 install: a handful of top-level drawers, a couple of
# levels under some of them, and a file-size distribution with a long tail.
SYNTH = [
    ("C", 60, 400, 24000),
    ("L", 12, 2000, 30000),
    ("Libs", 14, 3000, 90000),
    ("Devs", 10, 500, 40000),
    ("Devs/Printers", 8, 3000, 12000),
    ("Devs/Keymaps", 12, 300, 2000),
    ("S", 14, 100, 6000),
    ("Prefs", 18, 4000, 30000),
    ("Prefs/Presets", 10, 200, 3000),
    ("Fonts", 6, 500, 4000),
    ("Fonts/topaz", 4, 1500, 14000),
    ("Locale/Catalogs/deutsch", 20, 400, 9000),
    ("Locale/Languages", 6, 8000, 40000),
    ("Utilities", 12, 6000, 120000),
    ("System", 10, 8000, 220000),
    ("Storage/Monitors", 8, 3000, 20000),
    ("Tools/Commodities", 14, 3000, 16000),
    ("Expansion", 4, 1000, 8000),
]


def synth(dest, seed=20260729):
    x = seed & 0xFFFFFFFF or 1

    def rnd():
        nonlocal x
        x ^= (x << 13) & 0xFFFFFFFF
        x ^= x >> 17
        x ^= (x << 5) & 0xFFFFFFFF
        x &= 0xFFFFFFFF
        return x

    files = 0
    total = 0
    for drawer, count, lo, hi in SYNTH:
        d = os.path.join(dest, drawer)
        os.makedirs(d, exist_ok=True)
        for i in range(count):
            n = lo + rnd() % (hi - lo)
            body = bytearray(n)
            for j in range(0, n, 4):
                v = rnd()
                for k in range(min(4, n - j)):
                    body[j + k] = (v >> (8 * k)) & 0xFF
            with open(os.path.join(d, "file%02d" % i), "wb") as fh:
                fh.write(body)
            files += 1
            total += n
    return files, total


def disk_label(path):
    """The drawer an ADF's contents go under.

    Every disk of a set is named `Workbench v3.1 rev 40.42 (1994)(Commodore)
    (M10)(Disk 3 of 6)(Extras)[!].adf`, so anything taken off the front of that
    is the same string for all six and merges them into one drawer.  The last
    parenthesised group is what distinguishes them.
    """
    base = os.path.splitext(os.path.basename(path))[0]
    groups = [g for g in base.replace(")", "(").split("(") if g.strip()]
    for g in reversed(groups):
        g = g.strip().rstrip("!]").strip("[").strip()
        if g and not g.lower().startswith("disk ") and not g.isdigit():
            return g.replace(" ", "")
    return base[:20].replace(" ", "")


def main():
    if len(sys.argv) < 2:
        sys.stderr.write(__doc__)
        return 2

    dest = sys.argv[1]
    adfs = sys.argv[2:]
    os.makedirs(dest, exist_ok=True)

    if adfs:
        files = 0
        for a in adfs:
            files += ADF(a).extract(dest, None, disk_label(a))
        source = "%d ADF(s)" % len(adfs)
    else:
        files, _ = synth(dest)
        source = "synthesised"

    total = 0
    dirs = 0
    for root, dnames, fnames in os.walk(dest):
        dirs += len(dnames)
        for f in fnames:
            total += os.path.getsize(os.path.join(root, f))

    print("tree: %s, %d files in %d directories, %d bytes"
          % (source, files, dirs, total))
    return 0


if __name__ == "__main__":
    sys.exit(main())
