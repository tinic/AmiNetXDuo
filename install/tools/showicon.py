#!/usr/bin/env python3
"""Re-read a .info file the way icon.library does, and draw it.

Written against <workbench/workbench.h> independently of makeicon.py so that
the two disagreeing is a signal.  It walks every field in order and finishes
by checking that the file was consumed exactly -- an icon with bytes left
over, or one that runs short, is an icon icon.library will mis-read.

Usage: showicon.py <file.info> [...]

SPDX-License-Identifier: MIT
"""

import struct
import sys

TYPES = {1: "disk", 2: "drawer", 3: "tool", 4: "project", 5: "trashcan",
         6: "device", 7: "kickstart", 8: "appicon"}

# grey, black, white, blue
GLYPHS = " KWB"


def show(path):
    d = open(path, "rb").read()
    fail = []

    magic, version = struct.unpack(">HH", d[0:4])
    if magic != 0xE310:
        fail.append(f"do_Magic is {magic:#06x}, should be 0xe310")

    (_nxt, _le, _te, w, h, flags, activation, gtype,
     render, select, _gtext, _mx, _si, _gid, userdata) = \
        struct.unpack(">IhhhhHHHIIIiIHI", d[4:48])

    otype, _pad = struct.unpack(">BB", d[48:50])
    p_deftool, p_tooltypes = struct.unpack(">II", d[50:58])
    cx, cy = struct.unpack(">ii", d[58:66])
    p_drawer, p_toolwin = struct.unpack(">II", d[66:74])
    stack = struct.unpack(">i", d[74:78])[0]

    print(f"{path}")
    print(f"  type       {otype} ({TYPES.get(otype, '?')})   version {version}")
    print(f"  gadget     {w}x{h}  flags {flags:#06x}  "
          f"activation {activation:#06x}  type {gtype}  userdata {userdata}")
    print(f"  position   {cx:#010x},{cy:#010x}"
          f"{'  (NO_ICON_POSITION)' if cx == -0x80000000 else ''}")
    print(f"  stack      {stack}")

    if otype in (1, 2, 5) and not p_drawer:
        fail.append("a disk/drawer/trashcan icon needs DrawerData")
    if otype not in (1, 2, 5) and p_drawer:
        fail.append("DrawerData on an icon that is not a drawer")
    if not render:
        fail.append("no GadgetRender: the icon has no image")

    o = 78
    if p_drawer:
        o += 56

    def image(off):
        (le, te, iw, ih, depth, data, pick, onoff, nxt) = \
            struct.unpack(">hhhhhIBBI", d[off:off + 20])
        off += 20
        words = (iw + 15) // 16
        plane = words * 2 * ih
        px = [[0] * iw for _ in range(ih)]
        for pl in range(depth):
            base = off + pl * plane
            for y in range(ih):
                for wn in range(words):
                    at = base + y * words * 2 + wn * 2
                    v = struct.unpack(">H", d[at:at + 2])[0]
                    for b in range(16):
                        x = wn * 16 + b
                        if x < iw and (v >> (15 - b)) & 1:
                            px[y][x] |= 1 << pl
        return off + depth * plane, iw, ih, depth, pick, onoff, px

    o, iw, ih, depth, pick, onoff, px = image(o)
    print(f"  image      {iw}x{ih} depth {depth} "
          f"planepick {pick:#04x} planeonoff {onoff:#04x}")
    if (iw, ih) != (w, h):
        fail.append(f"image is {iw}x{ih} but the gadget says {w}x{h}")
    for row in px:
        print("   |" + "".join(GLYPHS[c] for c in row) + "|")

    if select:
        o, siw, sih, *_ = image(o)
        print(f"  select     {siw}x{sih}")

    if p_deftool:
        n = struct.unpack(">I", d[o:o + 4])[0]
        o += 4
        print(f"  defaulttool  {d[o:o + n - 1].decode('latin-1')!r}")
        o += n

    if p_tooltypes:
        total = struct.unpack(">I", d[o:o + 4])[0]
        o += 4
        for _ in range(total // 4 - 1):
            n = struct.unpack(">I", d[o:o + 4])[0]
            o += 4
            print(f"  tooltype     {d[o:o + n - 1].decode('latin-1')!r}")
            o += n

    if p_toolwin:
        n = struct.unpack(">I", d[o:o + 4])[0]
        o += 4 + n

    if p_drawer and userdata == 1:
        if len(d) - o < 6:
            fail.append("2.x drawer icon without its 6-byte DrawerData tail")
        else:
            ddflags, viewmodes = struct.unpack(">IH", d[o:o + 6])
            print(f"  drawer tail  flags {ddflags:#x} viewmodes {viewmodes:#x}")
            o += 6

    if o != len(d):
        fail.append(f"parsed {o} bytes of a {len(d)} byte file")

    for f in fail:
        print(f"  ** {f}")
    print("  ok" if not fail else "  FAILED")
    return not fail


if __name__ == "__main__":
    good = True
    for a in sys.argv[1:]:
        good = show(a) and good
        print()
    sys.exit(0 if good else 1)
