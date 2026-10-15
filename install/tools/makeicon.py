#!/usr/bin/env python3
"""Generate Amiga .info files with classic images and optional GlowIcons.

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

GlowIcon FORM ICON data follows the complete classic icon, including its
DrawerData tail. Older icon.library versions can still load the classic
image; color-capable libraries use the normal and selected color images.
The compiled artwork needs no image-processing dependencies at package time.

Usage:  makeicon.py <outdir> [--style glow|classic]

SPDX-License-Identifier: MIT
"""

import argparse
import math
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
               stack=4096, drawer=False, window=(60, 40, 450, 180)):
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
            *window,                        # LeftEdge TopEdge Width Height
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


def prefs_icon():
    """A network preferences panel: two sockets and three slider rows."""
    c = Canvas(48, 26, GREY)
    c.fill(2, 2, 45, 23, WHITE)
    c.bevel(1, 1, 46, 24)
    c.box(5, 5, 14, 12, BLACK)
    c.fill(7, 7, 12, 10, BLUE)
    c.box(33, 5, 42, 12, BLACK)
    c.fill(35, 7, 40, 10, BLUE)
    c.hline(15, 32, 8, BLACK)
    c.hline(8, 39, 16, BLACK)
    c.hline(8, 39, 20, BLACK)
    c.fill(17, 15, 21, 17, BLUE)
    c.box(17, 15, 21, 17, BLACK)
    c.fill(29, 19, 33, 21, BLUE)
    c.box(29, 19, 33, 21, BLACK)
    return c


def meter_icon():
    """A network meter: a panel with a receive and a send bar, each with its
    arrow, the receive bar fuller, as NetMeter's window usually is."""
    c = Canvas(48, 26, GREY)
    c.fill(2, 2, 45, 23, WHITE)
    c.bevel(1, 1, 46, 24)
    # Down arrow and the receive bar.
    for i in range(4):
        c.hline(5 + i, 11 - i, 6 + i, BLUE)
    c.box(14, 5, 42, 10, BLACK)
    c.fill(15, 6, 35, 9, BLUE)
    # Up arrow and the send bar.
    for i in range(4):
        c.hline(5 + i, 11 - i, 18 - i, BLACK)
    c.box(14, 15, 42, 20, BLACK)
    c.fill(15, 16, 24, 19, BLACK)
    return c


def switch_icon():
    """NetSwitch: three interface states -- a filled disc for online, a ring
    for offline, a square for not attached."""
    c = Canvas(48, 26, GREY)
    c.fill(2, 2, 45, 23, WHITE)
    c.bevel(1, 1, 46, 24)
    # Online: the filled disc.
    for r in (5, 4, 3, 2, 1, 0):
        _arc(c, 10, 13, r, 0, 360, BLUE)
    _arc(c, 10, 13, 5, 0, 360, BLACK)
    # Offline: the same disc, hollow.
    _arc(c, 24, 13, 5, 0, 360, BLACK)
    # Not attached: a hollow square.
    c.box(33, 8, 43, 18, BLACK)
    return c


def _arc(c, cx, cy, r, a0, a1, colour):
    """Points of a circle of radius r from angle a0 to a1 (degrees, 0 is
    right, 90 up), one pixel thick."""
    steps = max(8, int(r * abs(a1 - a0) / 20))
    for i in range(steps + 1):
        a = math.radians(a0 + (a1 - a0) * i / steps)
        c.set(int(round(cx + r * math.cos(a))),
              int(round(cy - r * 1.0 * math.sin(a))), colour)


def wireless_icon():
    """NetWireless: an antenna mast sending arcs to both sides."""
    c = Canvas(48, 26, GREY)
    c.fill(2, 2, 45, 23, WHITE)
    c.bevel(1, 1, 46, 24)
    c.vline(23, 12, 20, BLACK)
    c.vline(24, 12, 20, BLACK)
    c.hline(18, 29, 21, BLACK)
    c.fill(22, 10, 25, 12, BLUE)
    for r in (5, 9, 13):
        _arc(c, 23.5, 11, r, 145, 215, BLUE)
        _arc(c, 23.5, 11, r, -35, 35, BLUE)
    return c


def wireless_prefs_icon():
    """NetWirelessPrefs: Wi-Fi arcs beside a gear."""
    c = Canvas(48, 26, GREY)
    c.fill(2, 2, 45, 23, WHITE)
    c.bevel(1, 1, 46, 24)
    c.fill(13, 18, 15, 20, BLUE)
    for r in (4, 8, 12):
        _arc(c, 14, 19, r, 45, 135, BLUE)
    # The gear: a ring with eight teeth.
    for r in (5, 6):
        _arc(c, 35, 13, r, 0, 360, BLACK)
    for k in range(8):
        a = math.radians(k * 45)
        for r in (7, 8):
            c.set(int(round(35 + r * math.cos(a))),
                  int(round(13 - r * math.sin(a))), BLACK)
    c.fill(34, 12, 36, 14, BLUE)
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
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("outdir")
    parser.add_argument("--style", choices=("glow", "classic"), default="glow")
    args = parser.parse_args(argv[1:])
    outdir = args.outdir
    os.makedirs(outdir, exist_ok=True)

    icons = {
        "Install-AmiNetXDuo.info": diskobject(
            install_icon(), WBPROJECT,
            default_tool="Installer",
            tooltypes=[
                "APPNAME=AmiNetXDuo",
                "MINUSER=NOVICE",
                "DEFUSER=AVERAGE",
                "(The Installer uses a 16384 byte stack; this icon asks",
                " for it.  From a Shell, type \"Stack 16384\" first.)",
            ],
            stack=16384),
        # The same Installer project, pointed at the script that removes
        # AmiNetXDuo again; it is installed into the AmiNetXDuo drawer.
        "Uninstall-AmiNetXDuo.info": diskobject(
            install_icon(), WBPROJECT,
            default_tool="Installer",
            tooltypes=[
                "APPNAME=AmiNetXDuo",
                "SCRIPT=Uninstall-AmiNetXDuo",
                "MINUSER=NOVICE",
                "DEFUSER=AVERAGE",
            ],
            stack=16384),
        "AmiNetXDuo.info": diskobject(
            drawer_icon(), WBDRAWER, drawer=True, window=(10, 12, 620, 236)),
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
        "NetPrefs.info": diskobject(
            prefs_icon(), WBTOOL,
            tooltypes=[],
            stack=8192),
        # DONOTWAIT: NetMeter stays open, and from WBStartup Workbench would
        # otherwise wait for it to exit and then call it not responding.
        "NetMeter.info": diskobject(
            meter_icon(), WBTOOL,
            tooltypes=["DONOTWAIT"],
            stack=8192),
        # An action tool, not a status window: NetSwitch returns when its
        # window closes, so Workbench may wait for it.
        "NetSwitch.info": diskobject(
            switch_icon(), WBTOOL,
            tooltypes=[],
            stack=8192),
        # The Wi-Fi commodity: DONOTWAIT for WBStartup, as NetMeter.
        "NetWireless.info": diskobject(
            wireless_icon(), WBTOOL,
            tooltypes=["DONOTWAIT", "CX_PRIORITY=0",
                       "CX_POPKEY=ctrl alt w", "CX_POPUP=NO", "INTERVAL=5",
                       "APPICON=YES"],
            stack=8192),
        "NetWirelessPrefs.info": diskobject(
            wireless_prefs_icon(), WBTOOL,
            tooltypes=[],
            stack=8192),
    }

    for name, blob in icons.items():
        if args.style == "glow":
            artwork = {"Guide.info": "Document"}.get(name, name[:-5])
            path = os.path.join(os.path.dirname(__file__), "..", "icons",
                                "glow", artwork + ".glow")
            with open(path, "rb") as fh:
                color = fh.read()
            if (color[:4] != b"FORM" or color[8:12] != b"ICON" or
                    len(color) < 12 or
                    struct.unpack(">I", color[4:8])[0] != len(color) - 8):
                raise ValueError(f"invalid GlowIcon artwork: {path}")
            blob += color
        path = os.path.join(outdir, name)
        with open(path, "wb") as fh:
            fh.write(blob)
        print(f"{path}: {len(blob)} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
