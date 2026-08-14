#!/usr/bin/env python3
"""Connect to httpd's /console, decode what comes back, and say whether it is a screen.

    tests/tools/console-probe.py HOST PORT [--seconds N] [--png OUT.png]
                                [--pfs OUT.pfs] [--refresh] [--path /console]

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
CODE_RAW = 0
CODE_PB_RAW = 1
CODE_PB_XOR = 2


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
    def __init__(self, w, h, depth, bpr, tile_w, tile_h):
        self.w = w
        self.h = h
        self.depth = depth
        self.bpr = bpr
        self.tile_w = tile_w
        self.tile_h = tile_h
        self.across = (bpr + tile_w - 1) // tile_w
        self.down = (h + tile_h - 1) // tile_h
        self.plane = bpr * h
        self.planes = bytearray(self.plane * depth)
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
                for p in range(self.depth):
                    base = p * self.plane
                    rows = range(ch) if dy > 0 else range(ch - 1, -1, -1)
                    for r in rows:
                        to = base + (y0 + r) * self.bpr + x0
                        fr = base + (y0 + r + dy) * self.bpr + x0
                        dst[to:to + cw] = dst[fr:fr + cw]
                copies += 1
                continue

            if op != OP_TILE:
                raise Bad("op %d is not one of ours" % op)

            if i + 3 > len(b):
                raise Bad("a tile op is cut short")
            idx = (b[i] << 8) | b[i + 1]
            mask = b[i + 2]
            i += 3

            if idx >= self.across * self.down:
                raise Bad("tile index %d is off the grid" % idx)

            tx = idx % self.across
            ty = idx // self.across
            x0 = tx * self.tile_w
            y0 = ty * self.tile_h
            tw = min(self.tile_w, self.bpr - x0)
            th = min(self.tile_h, self.h - y0)
            want = tw * th

            for p in range(self.depth):
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
    """The frames as a .pfs, so tests/tools/pfs-check.py can read them too."""
    made(path)
    first = screens[0][0]
    blob = bytearray(b"PFS1")
    blob += struct.pack(">HHBBHHH", first.w, first.h, first.depth, 0,
                        first.bpr, len(screens), 0)
    blob += bytes(first.rgb)
    for _, planes in screens:
        blob += planes
    with open(path, "wb") as fh:
        fh.write(blob)


# ---------------------------------------------------------------- the run --

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
        elif argv[i] == "--refresh":
            want_refresh = True; i += 1
        else:
            print("unknown argument: %s" % argv[i], file=sys.stderr)
            return 2

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
                    if len(f) != 7:
                        raise Bad("geom takes six numbers: %r" % text)
                    n = [int(x) for x in f[1:]]
                    screen = Screen(n[0], n[1], n[2], n[3], n[4], n[5])
                    expect_seq = None
                    say("geom", " ".join(f[1:]))
                    say("frame_bytes", screen.plane * screen.depth)
                    if want_refresh:
                        wire.word("refresh")
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
            last = now
            if pfs_path is not None and len(kept) < 200:
                kept.append((screen, now))

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

    for p in problems:
        say("problem", p)

    say("RESULT", "PASS" if not problems else "FAIL")
    return 0 if not problems else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
