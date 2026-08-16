#!/usr/bin/env python3
"""Connect to httpd's /console, decode what comes back, and say whether it is a screen.

    tests/tools/console-probe.py HOST PORT [--seconds N] [--png OUT.png]
                                [--pfs OUT.pfs] [--refresh] [--path /console]
                                [--type TEXT] [--at N]
                                [--pointer "X,Y,B; X,Y,B; ..."] [--step N]
                                [--png-before OUT.png] [--reconnect N]

WHY IT DECODES RATHER THAN COUNTING

  A session that connects, upgrades and streams zeroes passes a connectivity
  check, a frame count and a byte total.  So this carries the whole receiver:
  the WebSocket framing, the `geom` and `pal` words, the encoder's ops -- COPY,
  RAW, PackBits and PackBits-over-XOR -- applied to a planar framebuffer, and
  then planar to chunky and out as a PNG.  What it asserts is that the pixels
  are a screen: more than one colour, a palette that is not all black, and a
  frame that changes when the guest's screen does.

  It is the same decode the browser does, written a second time on purpose.
  src/tools/web/client/console/tiles.ts is the one the person looks at; this
  one has never seen that file's output and agreeing with it is evidence.

--pointer PROVES THE MOUSE HALF

  A semicolon-separated list of `X,Y,BUTTONS` steps in AMIGA SCREEN PIXELS,
  each sent as the `m X Y B` word the browser sends, N frames apart.  A click
  is three steps -- move, press, release -- and a drag is a press, several
  moves with the button still held, and a release.  --png-before is written
  the moment the first step goes out and --png at the end, so what is asserted
  is that the picture changed where the pointer was, which is the only thing
  that distinguishes a mouse that works from one that lands somewhere else.

--reconnect PROVES THE SESSION SURVIVES BEING RESTARTED

  N+1 sessions in a row, each closed cleanly, and every one of them ends by
  asking the server what the screen really is: the copy built out of three
  hundred deltas, against the FULL FRAME the server sends for the same
  moment, decoded from zero.  An idle Workbench sends nothing once the first
  frame is out, so anything that corrupts the viewer's copy on a reconnect is
  never corrected and the screen simply stays wrong -- which is exactly how
  it shipped: the viewer asked for a refresh on every geom, the server
  answered a reconnect with a SECOND full frame, and the tiles being XOR
  against the shadow meant the second one cancelled the first back to an
  all-zero screen.  A connect-only check cannot see that.  This one compares
  pixels.

  IT DOES NOT COMPARE TWO SESSIONS TO EACH OTHER.  It used to, and that made
  every pixel the guest redrew on its own into a failure ten seconds later.
  Two pictures of ONE moment is the comparison that means something, and the
  cross-session difference is still printed -- as `screen_moved`, which is
  what it is.

--type PROVES THE INPUT HALF

  It sends Amiga rawkey codes for TEXT and then Return, once N frames have
  arrived, and reports how many of the frames after that differ from the one
  before them.  Typing `dir` at the Shell on the screen and then seeing the
  screen change is the whole assertion: nothing else on an idle machine moves
  a pixel, so a change after the keys and none before them is the keys having
  arrived.  The table is the one the browser sends,
  src/tools/web/client/console/rawkey.ts.

NO DEPENDENCIES

  The handshake, the frame codec and the PNG are all here, out of hashlib,
  base64, struct and zlib.  A harness that needs `pip install` is a harness
  that does not run.

Output is key=value; the exit code is the verdict.  0 pass, 1 a failed
assertion, 2 infrastructure -- nothing answered, the upgrade was refused.

SPDX-License-Identifier: MIT
"""

import base64
import hashlib
import os
import socket
import struct
import sys
import time
import zlib

GUID = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

RFB_VERSION = 1
OP_END = 0x00
OP_COPY = 0x01
OP_TILE = 0x02
OP_TILE8 = 0x03
CODE_RAW = 0
CODE_PB_RAW = 1
CODE_PB_XOR = 2


# KeyboardEvent.code to Amiga rawkey, the letters and the two keys this needs.
# Same numbers as src/tools/web/client/console/rawkey.ts, which is what a
# browser sends: a rawkey is a key position and the Amiga's own keymap is what
# turns it into a character.
# One character to the rawkey that produces it, and whether it needs shift.
# A rawkey is a key POSITION, so the colon is the semicolon key WITH the
# qualifier: sending 0x29 bare types a semicolon, which is what this table did
# before it carried the second half.  IEQUALIFIER_LSHIFT is 1.
QUAL_SHIFT = 1

RAWKEY = {
    "`": (0x00, 0), "1": (0x01, 0), "2": (0x02, 0), "3": (0x03, 0),
    "4": (0x04, 0), "5": (0x05, 0), "6": (0x06, 0), "7": (0x07, 0),
    "8": (0x08, 0), "9": (0x09, 0), "0": (0x0a, 0), "-": (0x0b, 0),
    "=": (0x0c, 0), "\\": (0x0d, 0),
    "q": (0x10, 0), "w": (0x11, 0), "e": (0x12, 0), "r": (0x13, 0),
    "t": (0x14, 0), "y": (0x15, 0), "u": (0x16, 0), "i": (0x17, 0),
    "o": (0x18, 0), "p": (0x19, 0), "[": (0x1a, 0), "]": (0x1b, 0),
    "a": (0x20, 0), "s": (0x21, 0), "d": (0x22, 0), "f": (0x23, 0),
    "g": (0x24, 0), "h": (0x25, 0), "j": (0x26, 0), "k": (0x27, 0),
    "l": (0x28, 0), ";": (0x29, 0), "'": (0x2a, 0),
    "z": (0x31, 0), "x": (0x32, 0), "c": (0x33, 0), "v": (0x34, 0),
    "b": (0x35, 0), "n": (0x36, 0), "m": (0x37, 0),
    ",": (0x38, 0), ".": (0x39, 0), "/": (0x3a, 0), " ": (0x40, 0),
    "\n": (0x44, 0),

    ":": (0x29, 1), '"': (0x2a, 1), "?": (0x3a, 1), "<": (0x38, 1),
    ">": (0x39, 1), "_": (0x0b, 1), "+": (0x0c, 1), "!": (0x01, 1),
    "@": (0x02, 1), "#": (0x03, 1), "$": (0x04, 1), "%": (0x05, 1),
    "^": (0x06, 1), "&": (0x07, 1), "*": (0x08, 1), "(": (0x09, 1),
    ")": (0x0a, 1), "{": (0x1a, 1), "}": (0x1b, 1), "|": (0x0d, 1),
    "~": (0x00, 1),
}
for _c in "abcdefghijklmnopqrstuvwxyz":
    RAWKEY[_c.upper()] = (RAWKEY[_c][0], 1)


def type_text(wire, text):
    """Down and up for every character, which is what a keyboard sends.

    A down with no up leaves the Amiga repeating it, and the qualifier goes on
    both halves because Intuition reads the shift state off the event rather
    than off a history of them.
    """
    n = 0
    for ch in text:
        kc = RAWKEY.get(ch)
        if kc is None:
            continue
        raw, shifted = kc
        qual = QUAL_SHIFT if shifted else 0
        wire.word("kd %d %d" % (raw, qual))
        wire.word("ku %d %d" % (raw, qual))
        n += 1
    return n


def say(k, v):
    print("%s=%s" % (k, v))
    sys.stdout.flush()


class Fault(Exception):
    """Infrastructure: nothing to assert about."""


class Bad(Exception):
    """The stream said something this decoder refuses."""


# ------------------------------------------------------------- the socket --

class Wire:
    """One WebSocket, client side, over a blocking socket with a deadline."""

    def __init__(self, host, port, path):
        self.buf = b""
        try:
            self.sock = socket.create_connection((host, port), timeout=10)
        except OSError as e:
            raise Fault("cannot connect to %s:%s: %s" % (host, port, e))
        self.sock.settimeout(20)
        self._handshake(host, port, path)

    def _handshake(self, host, port, path):
        key = base64.b64encode(os.urandom(16))
        want = base64.b64encode(hashlib.sha1(key + GUID).digest()).decode()

        req = (
            "GET %s HTTP/1.1\r\n"
            "Host: %s:%s\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: %s\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "\r\n" % (path, host, port, key.decode())
        )
        self.sock.sendall(req.encode())

        head = b""
        while b"\r\n\r\n" not in head:
            more = self.sock.recv(4096)
            if not more:
                raise Fault("the server closed during the handshake")
            head += more
            if len(head) > 65536:
                raise Fault("the handshake answer never ended")

        head, self.buf = head.split(b"\r\n\r\n", 1)
        lines = head.decode("iso-8859-1").split("\r\n")
        self.status = lines[0]

        if "101" not in self.status:
            raise Fault("the upgrade was refused: %s" % self.status)

        got = ""
        for line in lines[1:]:
            if line.lower().startswith("sec-websocket-accept:"):
                got = line.split(":", 1)[1].strip()
        if got != want:
            raise Fault("the accept value is %r, expected %r" % (got, want))

    def _fill(self, n):
        while len(self.buf) < n:
            more = self.sock.recv(65536)
            if not more:
                raise Fault("the server closed")
            self.buf += more

    def frame(self):
        """One message.  Returns (opcode, payload); control frames included."""
        while True:
            self._fill(2)
            b0, b1 = self.buf[0], self.buf[1]
            fin = b0 & 0x80
            op = b0 & 0x0F
            masked = b1 & 0x80
            length = b1 & 0x7F
            at = 2

            if length == 126:
                self._fill(4)
                length = struct.unpack(">H", self.buf[2:4])[0]
                at = 4
            elif length == 127:
                self._fill(10)
                length = struct.unpack(">Q", self.buf[2:10])[0]
                at = 10

            if masked:
                raise Bad("a server frame was masked, which RFC 6455 5.1 forbids")

            self._fill(at + length)
            payload = self.buf[at:at + length]
            self.buf = self.buf[at + length:]

            if not fin:
                raise Bad("this server is not expected to fragment")

            if op == 0x9:                       # ping
                self.send(0xA, payload)
                continue
            if op == 0xA:                       # pong
                continue
            return op, payload

    def send(self, op, payload):
        mask = os.urandom(4)
        body = bytes(b ^ mask[i & 3] for i, b in enumerate(payload))
        head = bytes([0x80 | op])
        n = len(payload)
        if n < 126:
            head += bytes([0x80 | n])
        elif n < 65536:
            head += bytes([0x80 | 126]) + struct.pack(">H", n)
        else:
            head += bytes([0x80 | 127]) + struct.pack(">Q", n)
        self.sock.sendall(head + mask + body)

    def word(self, text):
        self.send(0x1, text.encode())

    def close(self):
        try:
            self.send(0x8, struct.pack(">H", 1000))
        except OSError:
            pass
        try:
            self.sock.close()
        except OSError:
            pass


# ------------------------------------------------------------ the decoder --

def unpackbits(src, at, end, want):
    """IFF ILBM byte RLE.  Returns (bytes, where it ended)."""
    out = bytearray()
    i = at
    while len(out) < want:
        if i >= end:
            raise Bad("PackBits ran out of input")
        n = src[i]
        i += 1
        if n == 128:
            continue
        if n < 128:
            run = n + 1
            if i + run > end or len(out) + run > want:
                raise Bad("a PackBits literal of %d does not fit" % run)
            out += src[i:i + run]
            i += run
        else:
            run = 257 - n
            if i >= end or len(out) + run > want:
                raise Bad("a PackBits run of %d does not fit" % run)
            out += bytes([src[i]]) * run
            i += 1
    return bytes(out), i


class Screen:
    # fmt is rfb_geom.format as the geom word carries it: 0 planar, `depth`
    # one-bit planes; 1 chunky, ONE eight-bit plane whose bytes are palette
    # indices.  depth stays 8 there because it is what sizes the `pal`.
    def __init__(self, w, h, depth, bpr, tile_w, tile_h, fmt=0):
        self.w = w
        self.h = h
        self.depth = depth
        self.bpr = bpr
        self.tile_w = tile_w
        self.tile_h = tile_h
        self.fmt = fmt
        self.clut8 = (fmt == 1)
        self.nplanes = 1 if self.clut8 else depth
        self.across = (bpr + tile_w - 1) // tile_w
        self.down = (h + tile_h - 1) // tile_h
        self.plane = bpr * h
        self.planes = bytearray(self.plane * self.nplanes)
        self.rgb = bytearray(3 * (1 << depth))

    def apply(self, b):
        """One frame, in place.  Returns (seq, tiles, copies)."""
        if len(b) < 5:
            raise Bad("a frame is %d bytes" % len(b))
        if b[0] != RFB_VERSION:
            raise Bad("frame version %d, not %d" % (b[0], RFB_VERSION))

        seq = (b[2] << 8) | b[3]
        tiles = copies = 0
        i = 4
        dst = self.planes

        while True:
            if i >= len(b):
                raise Bad("a frame ended without an END op")
            op = b[i]
            i += 1

            if op == OP_END:
                break

            if op == OP_COPY:
                if i + 10 > len(b):
                    raise Bad("a copy op is cut short")
                x0, cw, y0, ch, dy = struct.unpack(">HHHHh", b[i:i + 10])
                i += 10
                if x0 + cw > self.bpr or y0 + ch > self.h:
                    raise Bad("a copy op leaves the screen")
                if y0 + dy < 0 or y0 + ch + dy > self.h:
                    raise Bad("a copy op reads from off the screen")
                for p in range(self.nplanes):
                    base = p * self.plane
                    rows = range(ch) if dy > 0 else range(ch - 1, -1, -1)
                    for r in rows:
                        to = base + (y0 + r) * self.bpr + x0
                        fr = base + (y0 + r + dy) * self.bpr + x0
                        dst[to:to + cw] = dst[fr:fr + cw]
                copies += 1
                continue

            if op not in (OP_TILE, OP_TILE8):
                raise Bad("op %d is not one of ours" % op)
            # Which op arrives is the geometry's answer and not a choice, so
            # the other one means the geom and the frames disagree about what
            # a byte is -- which draws a picture rather than failing.
            if (op == OP_TILE8) != self.clut8:
                raise Bad("op %d on a %s screen"
                          % (op, "chunky" if self.clut8 else "planar"))

            # No plane mask on a chunky tile: one plane, and it is the one
            # that changed.
            head = 2 if self.clut8 else 3
            if i + head > len(b):
                raise Bad("a tile op is cut short")
            idx = (b[i] << 8) | b[i + 1]
            mask = 1 if self.clut8 else b[i + 2]
            i += head

            if idx >= self.across * self.down:
                raise Bad("tile index %d is off the grid" % idx)

            tx = idx % self.across
            ty = idx // self.across
            x0 = tx * self.tile_w
            y0 = ty * self.tile_h
            tw = min(self.tile_w, self.bpr - x0)
            th = min(self.tile_h, self.h - y0)
            want = tw * th

            for p in range(self.nplanes):
                if not (mask & (1 << p)):
                    continue
                if i >= len(b):
                    raise Bad("a tile op ran out of planes")
                code = b[i]
                i += 1

                if code == CODE_RAW:
                    if i + want > len(b):
                        raise Bad("a raw tile is cut short")
                    src = b[i:i + want]
                    i += want
                elif code in (CODE_PB_RAW, CODE_PB_XOR):
                    if i + 2 > len(b):
                        raise Bad("a packed tile is cut short")
                    ln = (b[i] << 8) | b[i + 1]
                    i += 2
                    if i + ln > len(b):
                        raise Bad("a packed tile claims %d bytes and %d are left"
                                  % (ln, len(b) - i))
                    src, _ = unpackbits(b, i, i + ln, want)
                    i += ln
                else:
                    raise Bad("tile code %d is not one of ours" % code)

                base = p * self.plane
                for r in range(th):
                    to = base + (y0 + r) * self.bpr + x0
                    fo = r * tw
                    if code == CODE_PB_XOR:
                        for c in range(tw):
                            dst[to + c] ^= src[fo + c]
                    else:
                        dst[to:to + tw] = src[fo:fo + tw]

            tiles += 1

        return seq, tiles, copies

    def chunky(self):
        # Already chunky on a card: the bytes are the indices and the only
        # thing to do is drop the padding past the width.
        if self.clut8:
            out = bytearray(self.w * self.h)
            for y in range(self.h):
                o = y * self.w
                out[o:o + self.w] = self.planes[y * self.bpr:
                                                y * self.bpr + self.w]
            return out
        out = bytearray(self.w * self.h)
        for p in range(self.depth):
            base = p * self.plane
            bit = 1 << p
            for y in range(self.h):
                row = self.planes[base + y * self.bpr: base + (y + 1) * self.bpr]
                o = y * self.w
                for x in range(self.w):
                    if row[x >> 3] & (0x80 >> (x & 7)):
                        out[o + x] |= bit
        return out

    def png(self, path):
        made(path)
        pix = self.chunky()
        raw = bytearray()
        for y in range(self.h):
            raw.append(0)
            for v in pix[y * self.w:(y + 1) * self.w]:
                raw += self.rgb[v * 3:v * 3 + 3]

        def chunk(tag, payload):
            return (struct.pack(">I", len(payload)) + tag + payload +
                    struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))

        blob = b"\x89PNG\r\n\x1a\n"
        blob += chunk(b"IHDR",
                      struct.pack(">IIBBBBB", self.w, self.h, 8, 2, 0, 0, 0))
        blob += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
        blob += chunk(b"IEND", b"")
        with open(path, "wb") as fh:
            fh.write(blob)


def made(path):
    """The drawer the output goes in.  A probe that runs over ssh on another
    machine has no reason to expect the caller's tree to exist there, and a
    twelve-second capture ending in FileNotFoundError is a run thrown away."""
    d = os.path.dirname(os.path.abspath(path))
    if d:
        os.makedirs(d, exist_ok=True)


def pfs(path, screens):
    """The frames as a .pfs, so tests/tools/pfs-check.py can read them too.

    `screens` is (screen, planes, millis) per frame, and the milliseconds are
    when the frame ARRIVED -- a live session has no frame rate, so the file
    carries what happened rather than a cadence somebody has to guess at.
    There is no pointer image here: this probe never asked for one, so every
    frame names image 0.
    """
    made(path)
    first = screens[0][0]
    base = screens[0][2]
    blob = bytearray(b"PFS2")
    # Byte 9 is the .pfs flags byte, and bit 0 says the frames are chunky --
    # one eight-bit plane and not `depth` one-bit ones.  See pfs.ts.
    blob += struct.pack(">HHBBHHH", first.w, first.h, first.depth,
                        1 if first.clut8 else 0,
                        first.bpr, len(screens), 0)
    blob += bytes(first.rgb)
    for _, planes, _ms in screens:
        blob += planes
    last = 0
    for _, _planes, ms in screens:
        t = max(last, int(round((ms - base) * 1000.0)))
        last = t
        blob += struct.pack(">LhhHH", t, 0, 0, 0, 0)
    with open(path, "wb") as fh:
        fh.write(blob)


# ---------------------------------------------------------------- the run --

def differ(a, b, width):
    """How many pixels two chunky buffers disagree in, and where.

    The box matters more than the count: drift is corruption of whatever the
    encoder last touched and lands where the tiles are, and a screen that
    moved on its own lands where the thing that moved is.  A failure that
    prints x 240..470 y 0..7 is a title bar and a failure that prints the
    whole screen is not.
    """
    n = 0
    x0 = y0 = 1 << 30
    x1 = y1 = -1
    for i, (p, q) in enumerate(zip(a, b)):
        if p != q:
            n += 1
            x, y = i % width, i // width
            if x < x0: x0 = x
            if x > x1: x1 = x
            if y < y0: y0 = y
            if y > y1: y1 = y
    if n == 0:
        return 0, ""
    return n, "x %d..%d y %d..%d" % (x0, x1, y0, y1)


def refresh_to_truth(wire, screen, limit=8.0):
    """Ask for a full frame and decode it into a SECOND, empty screen.

    WHY THIS AND NOT TWO SESSIONS COMPARED TO EACH OTHER

      What a reconnect can break is this receiver's copy: a full frame XORed
      onto a picture that is already there, a shadow the two ends disagree
      about.  On an idle screen nothing afterwards corrects it, which is what
      makes it worth a check -- and also what made the old one compare two
      pictures taken ten seconds apart and call every difference drift.  A
      guest is a machine, not a photograph; anything that redraws on its own
      -- a title bar counting memory, a cursor -- fails that comparison while
      being exactly right.

      So the comparison is between two pictures of the SAME MOMENT, built two
      different ways.  `before` is what three hundred accumulated deltas say
      the screen is; `truth` is what the server's own full frame says it is,
      decoded from zero, milliseconds later.  The screen has no time to move
      between them, and a copy that drifted disagrees with truth however idle
      the guest is.

    Returns (before, truth, screen), the screen being the fresh one the
    session carries on from.
    """
    wire.word("refresh")
    deadline = time.time() + limit
    before = None
    fresh = None

    while time.time() < deadline:
        wire.sock.settimeout(max(1.0, deadline - time.time()))
        op, body = wire.frame()

        if op == 0x8:
            raise Bad("the server closed while it was answering a refresh")
        if op == 0x1:
            text = body.decode("iso-8859-1")
            if text.startswith("geom "):
                f = [int(x) for x in text.split()[1:]]
                if len(f) != 7:
                    raise Bad("geom takes seven numbers: %r" % text)
                # The reset lands here, so this is the last instant the
                # incremental copy is worth anything.
                if before is None:
                    before = bytes(screen.chunky())
                fresh = Screen(f[0], f[1], f[2], f[3], f[4], f[5], f[6])
                fresh.rgb = bytearray(screen.rgb)
            elif text.startswith("pal "):
                target = fresh if fresh is not None else screen
                target.rgb = bytearray.fromhex(text[4:].strip())
            continue
        if op != 0x2:
            raise Bad("opcode %d is not one this server sends" % op)

        if fresh is None:
            screen.apply(body)          # still the old stream; keep up with it
            continue

        fresh.apply(body)
        return before, bytes(fresh.chunky()), fresh

    raise Bad("the server never answered a refresh with a geometry and a"
              " full frame")


def verify_copy(wire, screen):
    """The copy this receiver built, against the server's own full frame.

    NOT RETRIED, AND NOT TOLERATED EITHER.

      Retrying is worthless here: the refresh that measures the copy also
      REPLACES it, so a second attempt only ever says that the first one's
      full frame arrived.  A corruption that happened at the start of the
      session -- which is what a reconnect defect is -- is gone by then.  So
      the first comparison is the verdict.

      Nor is there a tolerance.  The two pictures are one grab apart: the
      server sends a delta for anything that changes, so `before` is current
      up to the last frame it sent, and the full frame is encoded from the
      grab after that.  Nothing legitimate lands in a window that small
      often.

    WHEN IT DOES, THE GUEST SAYS SO ITSELF

      A difference is put to the guest rather than waved through: a SECOND
      full frame is asked for, and the server's own resync floor spaces it a
      second after the first.  Pixels that differ between two full frames a
      second apart are pixels the machine is actively redrawing -- a cursor,
      a title bar counting memory -- and drift is by definition none of
      those, because it is a copy that stopped tracking a screen that is
      standing still.  So the difference is reported as whatever is left
      after the moving pixels are taken out of it, and the moving ones are
      reported separately as what they are.

    Returns (pixels, box, live, screen); pixels 0 is a copy that is right.
    """
    before, truth, screen = refresh_to_truth(wire, screen)
    n, box = differ(before, truth, screen.w)
    if n == 0:
        return 0, "", 0, screen

    _before2, truth2, screen = refresh_to_truth(wire, screen)

    live = 0
    left = 0
    x0 = y0 = 1 << 30
    x1 = y1 = -1
    for i, (b, t) in enumerate(zip(before, truth)):
        if b == t:
            continue
        if truth[i] != truth2[i]:
            live += 1
            continue
        left += 1
        x, y = i % screen.w, i // screen.w
        if x < x0: x0 = x
        if x > x1: x1 = x
        if y < y0: y0 = y
        if y > y1: y1 = y

    if left == 0:
        return 0, "", live, screen
    return left, "x %d..%d y %d..%d" % (x0, x1, y0, y1), live, screen


class Shot:
    """What one session left behind: the picture, and whether it was right."""

    def __init__(self, pic, width, frames, words, geoms, drift, box, live):
        self.pic = pic
        self.width = width
        self.frames = frames
        self.words = words
        self.geoms = geoms
        self.drift = drift          # pixels the copy was wrong by; 0 is right
        self.box = box
        self.live = live            # pixels the guest was redrawing itself

    def verdict(self):
        moving = ("" if self.live == 0
                  else ", %d pixels the guest was redrawing" % self.live)
        if self.drift == 0:
            return "copy verified against a full frame" + moving
        return ("copy is %d pixels wrong (%s)%s"
                % (self.drift, self.box, moving))


def session_picture(host, port, path, seconds, refresh_at=4,
                    refresh_every_geom=False, verify=False):
    """One whole session, closed cleanly.  Returns a Shot.

    One `refresh` goes out mid-session, once the picture is already drawn.
    That is the hazard in one line: the answer to a refresh is a FULL frame,
    the tiles in it are XOR against the shadow, and a viewer that still holds
    the last picture when it lands XORs the two together and goes blank.  It
    is asked for here rather than left to a viewer to trip over.

    refresh_every_geom is the client nobody should write and somebody
    eventually will: the server answers a refresh with a geom, so asking on
    every geom is a request to be answered with the thing that provokes the
    request.  It must converge anyway, and the picture must be right at the
    end of it.
    """
    wire = Wire(host, port, path)
    screen = None
    frames = 0
    words = 0
    geoms = 0
    asked = False
    started = time.time()

    while time.time() - started < seconds:
        wire.sock.settimeout(max(1.0, seconds - (time.time() - started) + 5))
        op, body = wire.frame()

        if op == 0x8:
            break
        if op == 0x1:
            words += 1
            text = body.decode("iso-8859-1")
            if text.startswith("geom "):
                f = [int(x) for x in text.split()[1:]]
                if len(f) != 7:
                    raise Bad("geom takes seven numbers: %r" % text)
                screen = Screen(f[0], f[1], f[2], f[3], f[4], f[5], f[6])
                geoms += 1
                if refresh_every_geom:
                    wire.word("refresh")
            elif text.startswith("pal "):
                if screen is None:
                    raise Bad("a palette arrived before a geometry")
                screen.rgb = bytearray.fromhex(text[4:].strip())
            continue
        if op != 0x2:
            raise Bad("opcode %d is not one this server sends" % op)
        if screen is None:
            raise Bad("a frame arrived before a geometry")
        screen.apply(body)
        frames += 1

        if not asked and frames >= refresh_at:
            wire.word("refresh")
            asked = True

    drift = 0
    box = ""
    live = 0
    if screen is not None and verify:
        drift, box, live, screen = verify_copy(wire, screen)

    wire.close()
    if screen is None:
        raise Bad("no geometry word ever arrived")
    return Shot(bytes(screen.chunky()), screen.w, frames, words, geoms,
                drift, box, live)


def reconnect_check(host, port, path, times, seconds):
    """Connect, close, reconnect N times; no session's copy may be wrong.

    WHAT IS ASSERTED, AND WHY IT IS NOT "THE PICTURES MATCH"

      Every session ends by asking the server what the screen really is and
      comparing that against the copy it built out of deltas -- see
      refresh_to_truth().  That is the assertion, once per session, between
      two pictures of one moment.  A reconnect that corrupts the copy fails
      it in the session it happened in and says where.

      The pictures ARE still compared across sessions, and a difference is
      reported -- but as `screen_moved`, not as a failure, once every session
      involved has been shown to be a faithful copy.  Ten seconds apart on a
      running Amiga is long enough for a title bar to count some memory, and
      a check that calls that drift is a check that fails for being right.
    """
    shots = []
    try:
        for i in range(times + 1):
            s = session_picture(host, port, path, seconds, verify=True)
            shots.append(s)
            say("reconnect_%d" % i,
                "%d frames, %d words, %d distinct pixel values, %s"
                % (s.frames, s.words, len(set(s.pic)), s.verdict()))
            time.sleep(0.5)

        # And the receiver that asks on every geom.  The server answers a
        # refresh with a geom, so without a guard this is a loop that never
        # carries a frame; with one it re-syncs at the floor and converges.
        loop = session_picture(host, port, path, seconds,
                               refresh_every_geom=True, verify=True)
        say("refresh_loop",
            "%d frames, %d words, %d geoms, %d distinct pixel values, %s"
            % (loop.frames, loop.words, loop.geoms, len(set(loop.pic)),
               loop.verdict()))
    except Fault as e:
        say("error", e)
        say("RESULT", "INFRA")
        return 2
    except Bad as e:
        say("error", e)
        say("RESULT", "FAIL")
        return 1

    problems = []
    for i, s in enumerate(shots + [loop]):
        who = ("session %d" % i if i < len(shots)
               else "the receiver that refreshes on every geom")
        if len(set(s.pic)) < 2:
            problems.append("%s decoded to one pixel value: the screen went"
                            " blank and stayed blank" % who)
        if s.drift:
            problems.append("%s built a copy %d pixels away from the full"
                            " frame the server sent for the same moment, in"
                            " pixels the guest was not redrawing, at %s"
                            % (who, s.drift, s.box))
    if loop.frames == 0:
        problems.append("the receiver that refreshes on every geom was"
                        " never sent a frame: refresh is ping-ponging")

    # And what the sessions saw of each other, which is an observation about
    # the guest and not an assertion about this receiver: every copy above has
    # already been shown to be faithful, so a difference here is the screen
    # having changed between two sessions ten seconds apart.  Reported because
    # a bench that expects an idle machine wants to know when it did not get
    # one, and because it is the first thing to read when something below
    # DOES fail.
    for i, s in enumerate(shots[1:], 1):
        if s.pic != shots[0].pic:
            n, box = differ(s.pic, shots[0].pic, s.width)
            say("screen_moved", "session %d is %d pixels from session 0 (%s)"
                                % (i, n, box))

    for p in problems:
        say("problem", p)
    say("reconnect_sessions", len(shots))
    say("RESULT", "PASS" if not problems else "FAIL")
    return 0 if not problems else 1


def main(argv):
    if len(argv) < 3:
        print(__doc__.strip(), file=sys.stderr)
        return 2

    host = argv[1]
    port = int(argv[2])
    seconds = 10.0
    png = None
    pfs_path = None
    want_refresh = False
    path = "/console"
    typing = None
    type_at = 8
    pointer = None
    step = 4
    png_before = None
    reconnects = 0

    i = 3
    while i < len(argv):
        if argv[i] == "--seconds":
            seconds = float(argv[i + 1]); i += 2
        elif argv[i] == "--png":
            png = argv[i + 1]; i += 2
        elif argv[i] == "--pfs":
            pfs_path = argv[i + 1]; i += 2
        elif argv[i] == "--path":
            path = argv[i + 1]; i += 2
        elif argv[i] == "--type":
            typing = argv[i + 1]; i += 2
        elif argv[i] == "--at":
            type_at = int(argv[i + 1]); i += 2
        elif argv[i] == "--pointer":
            pointer = []
            for part in argv[i + 1].split(";"):
                part = part.strip()
                if not part:
                    continue
                f = [int(v) for v in part.split(",")]
                if len(f) != 3:
                    print("--pointer takes X,Y,BUTTONS steps", file=sys.stderr)
                    return 2
                pointer.append(tuple(f))
            i += 2
        elif argv[i] == "--step":
            step = int(argv[i + 1]); i += 2
        elif argv[i] == "--png-before":
            png_before = argv[i + 1]; i += 2
        elif argv[i] == "--reconnect":
            reconnects = int(argv[i + 1]); i += 2
        elif argv[i] == "--refresh":
            want_refresh = True; i += 1
        else:
            print("unknown argument: %s" % argv[i], file=sys.stderr)
            return 2

    if reconnects > 0:
        rc = reconnect_check(host, port, path, reconnects, seconds)
        if rc != 0:
            return rc

    try:
        wire = Wire(host, port, path)
    except Fault as e:
        say("error", e)
        say("RESULT", "INFRA")
        return 2

    say("handshake", "ok")

    screen = None
    frames = 0
    payload = 0
    words = 0
    expect_seq = None
    gaps = 0
    tiles = 0
    copies = 0
    fbstat = ""
    kept = []
    changed = 0
    changed_before = 0
    typed = 0
    refreshed = False
    pointed = 0
    point_at = None
    changed_before_pointer = 0
    last = None
    fault = None

    started = time.time()
    first_at = None

    try:
        while time.time() - started < seconds:
            wire.sock.settimeout(max(1.0, seconds - (time.time() - started) + 5))
            op, body = wire.frame()

            if op == 0x8:
                code = struct.unpack(">H", body[:2])[0] if len(body) >= 2 else 0
                say("server_close_code", code)
                say("server_close_reason", body[2:].decode("iso-8859-1", "replace"))
                break

            if op == 0x1:
                words += 1
                text = body.decode("iso-8859-1")
                if text.startswith("geom "):
                    f = text.split()
                    if len(f) != 8:
                        raise Bad("geom takes seven numbers: %r" % text)
                    n = [int(x) for x in f[1:]]
                    screen = Screen(n[0], n[1], n[2], n[3], n[4], n[5], n[6])
                    expect_seq = None
                    say("geom", " ".join(f[1:]))
                    say("frame_bytes", screen.plane * screen.nplanes)
                    # ONCE, and not on every geom.  A geom already means both
                    # sides are at zero -- the server clears its shadow
                    # whenever it queues one -- so a refresh here asks for a
                    # full frame that is already coming.  Worse, the server
                    # answers a refresh WITH a geom, so a receiver that asked
                    # on every one would ask forever.  The server coalesces
                    # that now, but this file is what the next client author
                    # reads, and it must not model the wrong thing.
                    if want_refresh and not refreshed:
                        wire.word("refresh")
                        refreshed = True
                elif text.startswith("pal "):
                    if screen is None:
                        raise Bad("a palette arrived before a geometry")
                    hexes = text[4:].strip()
                    want = 3 * (1 << screen.depth)
                    if len(hexes) != want * 2:
                        raise Bad("pal is %d bytes, depth %d needs %d"
                                  % (len(hexes) // 2, screen.depth, want))
                    screen.rgb = bytearray.fromhex(hexes)
                    say("palette_bytes", want)
                elif text.startswith("fbstat "):
                    fbstat = text[7:]
                else:
                    say("word_unknown", text[:60])
                continue

            if op != 0x2:
                raise Bad("opcode %d is not one this server sends" % op)

            if screen is None:
                raise Bad("a frame arrived before a geometry")

            if first_at is None:
                first_at = time.time()

            seq, t, c = screen.apply(body)
            frames += 1
            payload += len(body)
            tiles += t
            copies += c

            if expect_seq is not None and seq != expect_seq:
                gaps += 1
            expect_seq = (seq + 1) & 0xFFFF

            now = bytes(screen.planes)
            if last is not None and now != last:
                changed += 1
                if typing is not None and typed == 0:
                    changed_before += 1
                if pointer is not None and pointed == 0:
                    changed_before_pointer += 1
            last = now

            # One step every `step` TENTHS OF A SECOND.  Not every N frames:
            # a drag holds the screen's layer lock, the grab then reads a torn
            # frame or the encoder finds nothing to send, and a schedule that
            # counted frames would stop advancing in the middle of the drag
            # and never send the release.
            if pointer is not None and frames >= type_at:
                if point_at is None:
                    point_at = time.time()
                    if png_before is not None:
                        screen.png(png_before)
                        say("png_before", png_before)
                idx = int((time.time() - point_at) / (max(step, 1) * 0.1))
                while pointed <= idx and pointed < len(pointer):
                    x, y, b = pointer[pointed]
                    wire.word("m %d %d %d" % (x, y, b))
                    pointed += 1

            # Once the screen has settled, type.  Down and up for each key,
            # which is what a keyboard sends and what IECODE_UP_PREFIX is for;
            # a down with no up leaves the Amiga repeating it.
            if typing is not None and typed == 0 and frames >= type_at:
                typed = type_text(wire, typing + "\n")
                say("typed_keys", typed)
            if pfs_path is not None and len(kept) < 200:
                kept.append((screen, now, time.time()))

    except Fault as e:
        fault = ("INFRA", str(e))
    except Bad as e:
        fault = ("FAIL", str(e))
    except socket.timeout:
        fault = ("INFRA", "nothing arrived within the window")

    elapsed = time.time() - started
    wire.close()

    say("seconds", "%.2f" % elapsed)
    say("frames", frames)
    say("payload_bytes", payload)
    say("words", words)
    say("tile_ops", tiles)
    say("copy_ops", copies)
    say("seq_gaps", gaps)
    if elapsed > 0:
        say("fps", "%.2f" % (frames / elapsed))
        say("bytes_per_second", "%.0f" % (payload / elapsed))
    say("frames_changed", changed)
    if typing is not None:
        say("frames_changed_before_typing", changed_before)
        say("frames_changed_after_typing", changed - changed_before)
    if pointer is not None:
        say("pointer_steps_sent", pointed)
        say("frames_changed_before_pointer", changed_before_pointer)
        say("frames_changed_after_pointer", changed - changed_before_pointer)
    if fbstat:
        say("guest_fbstat", fbstat)

    if fault is not None:
        say("error", fault[1])
        say("RESULT", fault[0])
        return 2 if fault[0] == "INFRA" else 1

    if screen is None:
        say("error", "no geometry word ever arrived")
        say("RESULT", "INFRA")
        return 2

    pix = screen.chunky()
    distinct = len(set(pix))
    setpix = sum(1 for v in pix if v)
    say("distinct_pixel_values", distinct)
    say("set_pixels", setpix)
    say("palette_nonzero", sum(1 for b in screen.rgb if b))

    if png is not None:
        screen.png(png)
        say("png", png)
    if pfs_path is not None and kept:
        pfs(pfs_path, kept)
        say("pfs", pfs_path)

    # What separates a screen from a session that streamed zeroes: more than
    # one colour on it, a palette that is not all black, and no gap in the
    # sequence, since every delta after a gap is applied to bytes the encoder
    # did not think were there.
    problems = []
    if frames == 0:
        problems.append("no frames arrived")
    if distinct < 2:
        problems.append("the screen has %d distinct pixel value(s)" % distinct)
    if sum(screen.rgb) == 0:
        problems.append("the palette is entirely black")
    if gaps:
        problems.append("%d gap(s) in the sequence" % gaps)
    if pointer is not None:
        if pointed < len(pointer):
            problems.append("only %d of %d pointer steps went out: too few"
                            " frames arrived" % (pointed, len(pointer)))
        elif changed - changed_before_pointer == 0:
            problems.append("the screen did not change after %d pointer words"
                            " went out, so nothing reached input.device"
                            % pointed)
    if typing is not None:
        if typed == 0:
            problems.append("nothing was typed: too few frames arrived")
        elif changed - changed_before == 0:
            problems.append("the screen did not change after %d keys went out,"
                            " so nothing reached input.device" % typed)

    for p in problems:
        say("problem", p)

    say("RESULT", "PASS" if not problems else "FAIL")
    return 0 if not problems else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
