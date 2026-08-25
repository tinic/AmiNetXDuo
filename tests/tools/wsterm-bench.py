#!/usr/bin/env python3
# What a browser terminal on a 68020 actually costs.
# SPDX-License-Identifier: MIT

import base64
import os
import socket
import struct
import sys
import time

ADDR = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 8080

WAIT = float(os.environ.get("AMINETXDUO_WS_WAIT", "30"))


def frame(op, payload, fin=True):
    if isinstance(payload, str):
        payload = payload.encode("latin-1")
    mask = b"\x21\x43\x65\x87"
    b0 = (0x80 if fin else 0) | op
    n = len(payload)
    if n < 126:
        head = struct.pack("!BB", b0, 0x80 | n)
    else:
        head = struct.pack("!BBH", b0, 0x80 | 126, n)
    return head + mask + bytes(payload[i] ^ mask[i % 4] for i in range(n))


class Term:
    def __init__(self):
        began = time.time()
        self.s = socket.create_connection((ADDR, PORT), timeout=WAIT)
        key = base64.b64encode(b"0123456789abcdef").decode()
        self.s.sendall(("GET /shell HTTP/1.1\r\nHost: %s:%d\r\n"
                        "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                        "Sec-WebSocket-Key: %s\r\n"
                        "Sec-WebSocket-Version: 13\r\n\r\n"
                        % (ADDR, PORT, key)).encode())
        self.buf = b""
        while b"\r\n\r\n" not in self.buf:
            self.buf += self.s.recv(4096)
        head, self.buf = self.buf.split(b"\r\n\r\n", 1)
        self.status = int(head.split(b"\r\n")[0].split()[1])
        self.upgrade_ms = (time.time() - began) * 1000.0

    def read_until(self, want, seconds):
        """Everything said until `want` appears, and how long it took."""
        began = time.time()
        text = b""
        while time.time() - began < seconds:
            self.s.settimeout(max(0.05, seconds - (time.time() - began)))
            try:
                more = self.s.recv(8192)
            except socket.timeout:
                break
            if not more:
                break
            self.buf += more
            text += self.drain()
            if want is not None and want in text:
                break
        return text, time.time() - began

    def drain(self):
        """Whatever whole frames are in the buffer, as payload bytes."""
        out = b""
        while len(self.buf) >= 2:
            b0, b1 = self.buf[0], self.buf[1]
            n = b1 & 0x7f
            at = 2
            if n == 126:
                if len(self.buf) < 4:
                    break
                n = struct.unpack("!H", self.buf[2:4])[0]
                at = 4
            elif n == 127:
                if len(self.buf) < 10:
                    break
                n = struct.unpack("!Q", self.buf[2:10])[0]
                at = 10
            if len(self.buf) < at + n:
                break
            payload = self.buf[at:at + n]
            self.buf = self.buf[at + n:]
            if (b0 & 0x0f) in (0x0, 0x1, 0x2):
                out += payload
        return out

    def send(self, text):
        self.s.sendall(frame(0x2, text))

    def close(self):
        try:
            self.s.sendall(frame(0x8, struct.pack("!H", 1000)))
            self.s.close()
        except OSError:
            pass


def main():
    t = Term()
    if t.status != 101:
        print("result=infra")
        print("!! could not upgrade: %s" % t.status, file=sys.stderr)
        return 2

    print("upgrade_ms=%.0f" % t.upgrade_ms)

    banner, took = t.read_until(b">", WAIT)
    if b">" not in banner:
        print("result=infra")
        print("!! no prompt within %.0fs" % WAIT, file=sys.stderr)
        t.close()
        return 2
    print("prompt_ms=%.0f" % (took * 1000.0))

    rtts = []
    for i in range(10):
        tag = "BENCH%02d" % i
        began = time.time()
        t.send("Echo %s\n" % tag)
        got, _ = t.read_until(tag.encode(), WAIT)
        if tag.encode() not in got:
            print("result=infra")
            print("!! no answer to %s" % tag, file=sys.stderr)
            t.close()
            return 2
        rtts.append((time.time() - began) * 1000.0)

    rtts.sort()
    print("echo_rtt_ms_median=%.0f" % rtts[len(rtts) // 2])
    print("echo_rtt_ms_worst=%.0f" % rtts[-1])

    line = "X" * 200
    burst = 40
    for _ in range(burst):
        t.send("Echo %s\n" % line)

    began = time.time()
    total = 0
    deadline = began + WAIT
    while time.time() < deadline:
        t.s.settimeout(max(0.05, deadline - time.time()))
        try:
            more = t.s.recv(16384)
        except socket.timeout:
            break
        if not more:
            break
        t.buf += more
        total += len(t.drain())
        if total >= burst * (len(line) + 1):
            break
    elapsed = time.time() - began

    if elapsed > 0 and total > 0:
        print("out_bytes=%d" % total)
        print("out_seconds=%.2f" % elapsed)
        print("out_bytes_per_s=%.0f" % (total / elapsed))

    t.close()
    print("result=measured")
    return 0


if __name__ == "__main__":
    sys.exit(main())
