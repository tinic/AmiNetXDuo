#!/usr/bin/env python3
"""
The pattern FitzStress checks its bytes against, on the host side.

tests/stress/fitzstress.c generates the same bytes from the same seed, so a
file can be checked at whichever end it lands on and neither end has to send a
checksum through the stack under test.

    byte(o) = pat[o & 8191] ^ (o >> 13) & 0xff

`pat` is 8 KB of xorshift32 output.  The high-bits term makes the period 2 MB,
so a block that is repeated or dropped is caught as well as a byte that is
altered -- unless the displacement is an exact multiple of 2 MB.

    tests/stress/pattern.py write FILE BYTES [SEED]
    tests/stress/pattern.py check FILE [SEED]        exit 0 if it matches

SPDX-License-Identifier: MIT
"""

import sys

PAT_SIZE = 8192
CHUNK = 1 << 20


def pat_table(seed):
    x = seed & 0xFFFFFFFF or 2463534242
    out = bytearray(PAT_SIZE)
    for i in range(PAT_SIZE):
        x ^= (x << 13) & 0xFFFFFFFF
        x ^= x >> 17
        x ^= (x << 5) & 0xFFFFFFFF
        x &= 0xFFFFFFFF
        out[i] = x & 0xFF
    return bytes(out)


def block(table, off, length):
    """The pattern bytes for [off, off+length)."""
    out = bytearray(length)
    for i in range(length):
        o = off + i
        out[i] = table[o & (PAT_SIZE - 1)] ^ ((o >> 13) & 0xFF)
    return bytes(out)


def write(path, total, seed):
    table = pat_table(seed)
    with open(path, "wb") as fh:
        off = 0
        while off < total:
            n = min(CHUNK, total - off)
            fh.write(block(table, off, n))
            off += n


def check(path, seed, expect=None):
    """Returns (ok, size, first_bad_offset)."""
    table = pat_table(seed)
    off = 0
    with open(path, "rb") as fh:
        while True:
            buf = fh.read(CHUNK)
            if not buf:
                break
            want = block(table, off, len(buf))
            if buf != want:
                for i in range(len(buf)):
                    if buf[i] != want[i]:
                        return (False, off + len(buf), off + i)
            off += len(buf)
    if expect is not None and off != expect:
        return (False, off, -1)
    return (True, off, -1)


def main():
    if len(sys.argv) < 3:
        sys.stderr.write(__doc__)
        return 2

    mode = sys.argv[1]

    if mode == "write":
        seed = int(sys.argv[4]) if len(sys.argv) > 4 else 20260729
        write(sys.argv[2], int(sys.argv[3]), seed)
        return 0

    if mode == "check":
        seed = int(sys.argv[3]) if len(sys.argv) > 3 else 20260729
        ok, size, bad = check(sys.argv[2], seed)
        if ok:
            print("ok %s %d bytes" % (sys.argv[2], size))
            return 0
        print("MISMATCH %s at byte %d (size %d)" % (sys.argv[2], bad, size))
        return 1

    sys.stderr.write("unknown mode %s\n" % mode)
    return 2


if __name__ == "__main__":
    sys.exit(main())
