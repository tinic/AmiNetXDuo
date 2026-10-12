#!/usr/bin/env python3
"""Re-read a .info file the way icon.library does, and draw it.

Written against <workbench/workbench.h> independently of makeicon.py so that
the two disagreeing is a signal.  It walks every field in order and finishes
by checking that the file was consumed exactly. A trailing GlowIcon FORM
ICON is validated separately, including both image streams and palettes.

Usage: showicon.py <file.info> [...]

SPDX-License-Identifier: MIT
"""

import struct
import sys

TYPES = {1: "disk", 2: "drawer", 3: "tool", 4: "project", 5: "trashcan",
         6: "device", 7: "kickstart", 8: "appicon"}

# grey, black, white, blue
GLYPHS = " KWB"


def unpack_bytes(data, expected, compressed):
    """Decode the 8-bit PackBits variant used by our GlowIcon artwork."""
    if not compressed:
        if len(data) != expected:
            raise ValueError("incorrect uncompressed GlowIcon data length")
        return data
    out = bytearray()
    off = 0
    while off < len(data):
        control = data[off]
        off += 1
        if control == 128:
            continue
        count = control + 1 if control < 128 else 257 - control
        needed = count if control < 128 else 1
        if off + needed > len(data):
            raise ValueError("truncated GlowIcon RLE packet")
        if control < 128:
            out += data[off:off + count]
        else:
            out += data[off:off + 1] * count
        off += needed
        if len(out) > expected:
            raise ValueError("GlowIcon RLE exceeds image or palette size")
    if len(out) != expected:
        raise ValueError("incomplete GlowIcon RLE stream")
    return bytes(out)


def read_glow(data):
    """Strict reader for the FACE/two 8-bit IMAG form shipped by this tree.

    Does not import the writer: rejects trailing/truncated chunks, invalid
    palette indices, missing selected states and unsupported encodings.
    """
    if len(data) < 12 or data[:4] != b"FORM" or data[8:12] != b"ICON":
        raise ValueError("trailing data is not a GlowIcon FORM ICON")
    if struct.unpack(">I", data[4:8])[0] != len(data) - 8:
        raise ValueError("GlowIcon FORM length does not match the file")
    off = 12
    face = None
    images = []
    while off < len(data):
        if off + 8 > len(data):
            raise ValueError("truncated GlowIcon chunk header")
        kind, size = struct.unpack(">4sI", data[off:off + 8])
        end = off + 8 + size
        if end + (size & 1) > len(data):
            raise ValueError("truncated GlowIcon chunk or padding")
        chunk = data[off + 8:end]
        off = end + (size & 1)
        if kind == b"FACE":
            if face is not None or images or size != 6:
                raise ValueError("invalid or repeated GlowIcon FACE")
            width, height, flags, aspect, max_palette = struct.unpack(">BBBBH", chunk)
            if flags != 1 or aspect != 0:
                raise ValueError("unsupported GlowIcon FACE flags or aspect")
            face = (width + 1, height + 1, max_palette + 1)
        elif kind == b"IMAG":
            if face is None or size < 10 or len(images) == 2:
                raise ValueError("GlowIcon IMAG without FACE, truncated or excess")
            trans, colors, flags, image_format, palette_format, depth, ni, np = \
                struct.unpack(">BBBBBBHH", chunk[:10])
            colors += 1
            ni += 1
            np += 1
            if flags != 3 or depth != 8 or image_format not in (0, 1) or palette_format not in (0, 1):
                raise ValueError("unsupported GlowIcon image encoding")
            if trans >= colors or colors * 3 > face[2] or size != 10 + ni + np:
                raise ValueError("invalid GlowIcon palette or stream sizes")
            pixels = unpack_bytes(chunk[10:10 + ni], face[0] * face[1], image_format)
            palette = unpack_bytes(chunk[10 + ni:], colors * 3, palette_format)
            if any(pixel >= colors for pixel in pixels):
                raise ValueError("GlowIcon pixel outside its palette")
            images.append(dict(width=face[0], height=face[1], pixels=pixels,
                               palette=palette, transparent=trans))
        else:
            raise ValueError(f"unsupported GlowIcon chunk {kind!r}")
    if len(images) != 2:
        raise ValueError("GlowIcon needs normal and selected images")
    return images


def show(path):
    with open(path, "rb") as stream:
        d = stream.read()
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
        try:
            states = read_glow(d[o:])
            for name, state in zip(("normal", "selected"), states):
                print(f"  glow {name:8s} {state['width']}x{state['height']} "
                      f"{len(state['palette']) // 3} colors, transparent pen {state['transparent']}")
        except ValueError as error:
            fail.append(str(error))

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
