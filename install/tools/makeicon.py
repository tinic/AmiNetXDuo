#!/usr/bin/env python3
"""Generate genuine AmigaOS 2.x .info (icon) files.

The .info format is a `struct DiskObject` written big-endian, followed by the
things its pointers say are present, in a fixed order.  Everything here comes
from <workbench/workbench.h> and the icon.library autodocs:

    struct DiskObject           78 bytes
      UWORD do_Magic            0xE310
      UWORD do_Version          1
      struct Gadget do_Gadget   44 bytes
      UBYTE do_Type             1 disk 2 drawer 3 tool 4 project 5 trashcan
      UBYTE pad
      APTR  do_DefaultTool      non-zero => a string follows
      APTR  do_ToolTypes        non-zero => a string array follows
      LONG  do_CurrentX/Y       0x80000000 = NO_ICON_POSITION
      APTR  do_DrawerData       non-zero => 56 bytes of DrawerData follow
      APTR  do_ToolWindow       non-zero => a string follows
      LONG  do_StackSize

    then, in this order:
      DrawerData (56)                  if do_DrawerData
      Image + planes for GadgetRender  if do_Gadget.GadgetRender
      Image + planes for SelectRender  if do_Gadget.SelectRender
      LONG len + DefaultTool           if do_DefaultTool
      LONG (n+1)*4, then LONG len + string per entry, for ToolTypes
      LONG len + ToolWindow            if do_ToolWindow
      LONG dd_Flags + UWORD dd_ViewModes   if DrawerData and UserData == 1

The last line is the one that is easy to miss: an OS 2.x drawer icon marks
itself with do_Gadget.UserData == 1 and carries six extra bytes at the very
end of the file.  Icons that claim to be 2.x and omit them make icon.library
read past the end.

Bitmaps are written as planar data, plane 0 first, each row padded to a
whole number of 16-bit words, the Amiga's native bitmap layout.

Usage:  makeicon.py <outdir>

SPDX-License-Identifier: MIT
"""

import os
import struct
import sys

# ---------------------------------------------------------------- palette --
#
# The four colours of a standard Workbench 2.x icon.

GREY, BLACK, WHITE, BLUE = 0, 1, 2, 3

NO_ICON_POSITION = 0x80000000

# workbench.h object types
WBDISK, WBDRAWER, WBTOOL, WBPROJECT, WBGARBAGE = 1, 2, 3, 4, 5

# intuition gadget bits
GADGIMAGE = 0x0004
GADGHCOMP = 0x0000
GACT_RELVERIFY = 0x0001
GACT_IMMEDIATE = 0x0002
GTYP_BOOLGADGET = 0x0001


class Canvas:
    """A tiny colour-index raster with just enough drawing to make an icon."""

    def __init__(self, width, height, colour=GREY):
        self.w = width
        self.h = height
        self.px = [[colour] * width for _ in range(height)]

    def set(self, x, y, c):
        if 0 <= x < self.w and 0 <= y < self.h:
            self.px[y][x] = c

    def hline(self, x0, x1, y, c):
        for x in range(x0, x1 + 1):
            self.set(x, y, c)

    def vline(self, x, y0, y1, c):
        for y in range(y0, y1 + 1):
            self.set(x, y, c)

    def box(self, x0, y0, x1, y1, c):
        self.hline(x0, x1, y0, c)
        self.hline(x0, x1, y1, c)
        self.vline(x0, y0, y1, c)
        self.vline(x1, y0, y1, c)

    def fill(self, x0, y0, x1, y1, c):
        for y in range(y0, y1 + 1):
            self.hline(x0, x1, y, c)

    def bevel(self, x0, y0, x1, y1, light=WHITE, dark=BLACK):
        """The 3D edge every Amiga icon has: lit from the top left."""
        self.hline(x0, x1 - 1, y0, light)
        self.vline(x0, y0, y1 - 1, light)
        self.hline(x0 + 1, x1, y1, dark)
        self.vline(x1, y0 + 1, y1, dark)

    def stamp(self, x0, y0, art, mapping):
        """Draw a block of characters; a space leaves the pixel alone."""
        for dy, row in enumerate(art):
            for dx, ch in enumerate(row):
                if ch != " ":
                    self.set(x0 + dx, y0 + dy, mapping[ch])

    def planes(self, depth=2):
        """Planar bitmap data: plane 0 first, rows padded to 16 bits."""
        words = (self.w + 15) // 16
        out = bytearray()
        for plane in range(depth):
            bit = 1 << plane
            for y in range(self.h):
                row = self.px[y]
                for wnum in range(words):
                    value = 0
                    for b in range(16):
                        x = wnum * 16 + b
                        value <<= 1
                        if x < self.w and (row[x] & bit):
                            value |= 1
                    out += struct.pack(">H", value)
        return bytes(out)


# ------------------------------------------------------------ file writer --

def diskobject(canvas, obj_type, default_tool=None, tooltypes=(),
               stack=4096, drawer=False):
    w, h = canvas.w, canvas.h

    gadget = struct.pack(
        ">IhhhhHHHIIIiIHI",
        0,                                  # NextGadget
        0, 0,                               # LeftEdge, TopEdge
        w, h,                               # Width, Height
        GADGIMAGE | GADGHCOMP,              # Flags
        GACT_RELVERIFY | GACT_IMMEDIATE,    # Activation
        GTYP_BOOLGADGET,                    # GadgetType
        0x0000ABC0,                         # GadgetRender: present
        0,                                  # SelectRender: absent
        0,                                  # GadgetText
        0,                                  # MutualExclude
        0,                                  # SpecialInfo
        0,                                  # GadgetID
        1 if drawer else 0)                 # UserData: 1 = OS 2.x icon
    assert len(gadget) == 44, len(gadget)

    do = struct.pack(">HH", 0xE310, 1)
    do += gadget
    do += struct.pack(">BB", obj_type, 0)
    do += struct.pack(">I", 0x0000ABC4 if default_tool is not None else 0)
    do += struct.pack(">I", 0x0000ABC8 if tooltypes else 0)
    do += struct.pack(">ii", NO_ICON_POSITION - (1 << 32),
                      NO_ICON_POSITION - (1 << 32))
    do += struct.pack(">I", 0x0000ABCC if drawer else 0)
    do += struct.pack(">I", 0)              # ToolWindow
    do += struct.pack(">i", stack)
    assert len(do) == 78, len(do)

    out = bytearray(do)

    if drawer:
        # struct DrawerData: a NewWindow (48) plus dd_CurrentX/dd_CurrentY.
        nw = struct.pack(
            ">hhhhBBIIIIIIIhhHHH",
            50, 40, 300, 150,               # LeftEdge TopEdge Width Height
            255, 255,                       # DetailPen, BlockPen
            0,                              # IDCMPFlags
            0x0004022F,                     # Flags: the usual drawer window
            0, 0, 0, 0, 0,                  # gadgets checkmark title screen bm
            94, 65,                         # MinWidth, MinHeight
            0x7FFF, 0x7FFF,                 # MaxWidth, MaxHeight
            1)                              # Type = WBENCHSCREEN
        assert len(nw) == 48, len(nw)
        out += nw
        out += struct.pack(">ii", 0, 0)     # dd_CurrentX, dd_CurrentY

    image = struct.pack(
        ">hhhhhIBBI",
        0, 0,                               # LeftEdge, TopEdge
        w, h,                               # Width, Height
        2,                                  # Depth
        0x0000ABD0,                         # ImageData: present
        0x03,                               # PlanePick: planes 0 and 1
        0x00,                               # PlaneOnOff
        0)                                  # NextImage
    assert len(image) == 20, len(image)
    out += image
    out += canvas.planes(2)

    if default_tool is not None:
        blob = default_tool.encode("latin-1") + b"\0"
        out += struct.pack(">I", len(blob)) + blob

    if tooltypes:
        out += struct.pack(">I", (len(tooltypes) + 1) * 4)
        for tt in tooltypes:
            blob = tt.encode("latin-1") + b"\0"
            out += struct.pack(">I", len(blob)) + blob

    if drawer:
        # OS 2.x DrawerData tail, present because UserData == 1.
        out += struct.pack(">IH", 0, 0)     # dd_Flags, dd_ViewModes

    return bytes(out)


# ----------------------------------------------------------------- icons ---

# The installer: the motif every Amiga user knows from Install3.1, an arrow
# with a check mark going down into a slot.  Our own pixels; Commodore's icon
# is not ours to ship.
INSTALL_ART = [
    "................................................................",
    "........................KKKKKKKKKKKKKKKKK.......................",
    "........................KBBBBBBBBBBBBBBBK.......................",
    "........................KBBBBBBBBBBBBBWWK.......................",
    "........................KBBBBBBBBBBBWWWWK.......................",
    "........................KBWWWBBBBBWWWWWBK.......................",
    "........................KBWWWWWBWWWWWBBBK.......................",
    "........................KBBBWWWWWWWBBBBBK.......................",
    "........................KBBBBBWWWBBBBBBBK.......................",
    "................KKKKKKKKBBBBBBBBBBBBBBBBBKKKKKKKK...............",
    "..................KKBBBBBBBBBBBBBBBBBBBBBBBBBKK.................",
    "....................KKBBBBBBBBBBBBBBBBBBBBBKK...................",
    "......................KKBBBBBBBBBBBBBBBBBKK.....................",
    "..WWWWWWWWWWWWWWWWWWWWWWKKBBBBBBBBBBBBBKKWWWWWWWWWWWWWWWWWWWW...",
    "..W.......................KKBBBBBBBBBKK......................K..",
    "..W...KKKKKKKKKKKKKKKKKKKKKKKKBBBBBKKKKKKKKKKKKKKKKKKKKKKK...K..",
    "..W...KKKKKKKKKKKKKKKKKKKKKKKKKKBKKKKKKKKKKKKKKKKKKKKKKKKK...K..",
    "..W...WWWWWWWWWWWWWWWWWWWWWWWWWWKWWWWWWWWWWWWWWWWWWWWWWWWW...K..",
    "..W..........................................................K..",
    "..W..........................................................K..",
    "..W.................................................KKKK.....K..",
    "..W.................................................WWWW.....K..",
    "..W..........................................................K..",
    "..W..........................................................K..",
    "...KKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKK..",
    "................................................................",
]


def install_icon():
    c = Canvas(64, 26, GREY)
    c.stamp(0, 0, INSTALL_ART,
            {".": GREY, "K": BLACK, "W": WHITE, "B": BLUE})
    return c


# The drawer front: a globe on a bus between two machines, with the protocol
# on a plate underneath.  Drawn for a 640x256 Workbench, where a hires pixel
# is about half as wide as it is tall, so the globe is 26 pixels across and 13
# down.  The name is not on it; Workbench writes that under the icon.
DRAWER_ART = [
    ".............................KKKKKK.............................",
    ".........................KKKKBBBKBBKKKK.........................",
    "......................KKWWWWBBBBKBWWWWWWKK......................",
    ".....................KBWWWWWWBBBKWWWWWWWWBK.....................",
    "WWWWWWWWWWWWW.......KBBBWWWWBBBBKBWWWWWWWBBK.....WWWWWWWWWWWWW..",
    "W............K......KBBBBBWWBBBBKBBBWWWWBBBK.....W............K.",
    "W..KKKKKKKK..K.....KKKKKKKKWWWKKKKKWWWKKKKKKK....W..KKKKKKKK..K.",
    "W..KWBBBBBB..K......KBBBBBBBWWBBKBBWWWBBBBBK.....W..KWBBBBBB..K.",
    "W..KBBBBBBB..K......KBBBBBBBWBBBKBBBWBBBWWBK.....W..KBBBBBBB..K.",
    "W..KBBBBBBB..K.......KBBBBBBBBBBKBBBBBBBBBK......W..KBBBBBBB..K.",
    "W..KBBBBBBB..K........KKKBBBBBBBKBBBBBBKKK.......W..KBBBBBBB..K.",
    "W............K...........KKKKWWWWW.KKKK..........W............K.",
    ".KKKKKKKKKKKKK...............W....K...............KKKKKKKKKKKKK.",
    "WWWWWWWWWWWWWW.KKKKKKKKKKKKKKW....KKKKKKKKKKKKKKKWWWWWWWWWWWWWW.",
    "W..KKKKK......KWWWWWWWWWWWWWWW....KWWWWWWWWWWWWWWW..KKKKK......K",
    ".KKKKKKKKKKKKKK...............KKKKK...............KKKKKKKKKKKKKK",
    "............WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW.............",
    "............W......................................K............",
    "............W.KKKKK..KKK..KKKK......K.KKKKK.KKKK...K............",
    "............W...K...K...K.K...K.....K...K...K...K..K............",
    "............W...K...K.....K...K....K....K...K...K..K............",
    "............W...K...K.....KKKK....K.....K...KKKK...K............",
    "............W...K...K.....K......K......K...K......K............",
    "............W...K...K...K.K.....K.......K...K......K............",
    "............W...K....KKK..K.....K.....KKKKK.K......K............",
    ".............KKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKK............",
]


def drawer_icon():
    c = Canvas(64, 26, GREY)
    c.stamp(0, 0, DRAWER_ART,
            {".": GREY, "K": BLACK, "W": WHITE, "B": BLUE})
    return c


def document_icon():
    """A project icon for a text file: a sheet of paper with lines on it."""
    c = Canvas(40, 26, GREY)

    c.fill(5, 1, 34, 24, WHITE)
    c.box(5, 1, 34, 24, BLACK)

    # The folded corner: the fold is a triangle of GREY with a black diagonal,
    # and the sheet's own outline is broken so the corner reads as turned over
    # rather than as a grey square pasted on.
    for i in range(9):
        for x in range(34 - 8 + i, 35):
            c.set(x, 1 + i, GREY)
        c.set(34 - 8 + i, 1 + i, BLACK)
    c.hline(34 - 8, 34, 1, GREY)
    c.vline(34, 1, 8, GREY)
    c.hline(26, 33, 9, BLACK)

    for y in range(12, 22, 3):
        c.hline(9, 30, y, BLUE)
    c.hline(9, 23, 21, BLUE)
    return c


# The stock Workbench 3.1 drawer, a bevelled front with a handle, for Docs
# and Examples: a drawer that holds files should look like every other one.
STOCK_DRAWER_ART = [
    "..WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW....",
    "..KWKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKK...",
    "..KWKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKWK...",
    "..KWKWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWKWK...",
    "..KWKWWWWWWWWWWWWWWWWKKKKKKKKKKKKKWWWWWWWWWWWWWWWWWKWK...",
    "..KWKWWWWWWWWWWWWWWWWKKKKKKKKKKKKKWWWWWWWWWWWWWWWWWKWK...",
    "..KWKWWWWWWWWWWWWWWWWKKWKWKWKWKWKKWWWWWWWWWWWWWWWWWKWK...",
    "..KWKWWWWWWWWWWWWWWWWKKKKKKKKKKKKKWWWWWWWWWWWWWWWWWKWK...",
    "..KWKWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWKWK...",
    "..KWKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKWK...",
    "..KWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWK...",
    "..KWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWK...",
    "....KKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKK...",
    ".........................................................",
]


def plain_drawer_icon():
    c = Canvas(57, 14, GREY)
    c.stamp(0, 0, STOCK_DRAWER_ART,
            {".": GREY, "K": BLACK, "W": WHITE, "B": BLUE})
    return c


def main(argv):
    if len(argv) != 2:
        print("usage: makeicon.py <outdir>", file=sys.stderr)
        return 2
    outdir = argv[1]
    os.makedirs(outdir, exist_ok=True)

    icons = {
        "Install-AmiNetXDuo.info": diskobject(
            install_icon(), WBPROJECT,
            default_tool="Installer",
            tooltypes=[
                "APPNAME=AmiNetXDuo",
                "MINUSER=NOVICE",
                "DEFUSER=AVERAGE",
                "(The Installer needs a 10000 byte stack; this icon asks",
                " for it.  From a Shell, type \"Stack 10000\" first.)",
            ],
            stack=10000),
        "AmiNetXDuo.info": diskobject(
            drawer_icon(), WBDRAWER, drawer=True),
        "Drawer.info": diskobject(
            plain_drawer_icon(), WBDRAWER, drawer=True),
        "Document.info": diskobject(
            document_icon(), WBPROJECT,
            default_tool="SYS:Utilities/More",
            tooltypes=[],
            stack=4096),
        # The same sheet of paper, pointed at MultiView: an AmigaGuide opened
        # in More is its own markup, which is not what "double-click it" in
        # the ReadMe means.
        "Guide.info": diskobject(
            document_icon(), WBPROJECT,
            default_tool="SYS:Utilities/MultiView",
            tooltypes=[],
            stack=8192),
    }

    for name, blob in icons.items():
        path = os.path.join(outdir, name)
        with open(path, "wb") as fh:
            fh.write(blob)
        print(f"{path}: {len(blob)} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
