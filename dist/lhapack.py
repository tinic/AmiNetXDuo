#!/usr/bin/env python3
"""Write an LHA archive without needing an LHA that can write one.

macOS ships, and Homebrew installs, Lhasa, which extracts LHA archives and
cannot create them.  This produces a valid archive so that `make-dist.sh`
works on a machine with no archiver at all.

What it writes: header level 1, method "-lh0-" (stored, no compression),
CRC-16/ARC over the file data, and the directory in an extended header of
type 0x02 with 0xFF between components, which is where every LHA since the
early nineties expects to find it.  Level 0 was tried first and rejected:
its convention of packing the whole path into the name field with 0xFF
separators is not universally decoded (Lhasa does not), so the paths come
out mangled.

What it does NOT do: compress.  A real `lha a` produces -lh5- and an archive
roughly half the size.  make-dist.sh therefore prefers a real archiver when
it finds one and only falls back to this.  If you are producing an archive
for upload, use a machine with a proper LhA on it.

Usage:  lhapack.py <archive.lha> <basedir> [member ...]

Members are paths relative to basedir; with none given, everything under
basedir is archived.  Order is preserved, which matters only aesthetically.

SPDX-License-Identifier: MIT
"""

import os
import struct
import sys
import time

CRC16_TABLE = []
for _byte in range(256):
    _crc = _byte
    for _ in range(8):
        _crc = (_crc >> 1) ^ (0xA001 if _crc & 1 else 0)
    CRC16_TABLE.append(_crc)


def crc16(data):
    crc = 0
    for b in data:
        crc = (crc >> 8) ^ CRC16_TABLE[(crc ^ b) & 0xFF]
    return crc


def dos_time(epoch):
    t = time.localtime(epoch)
    year = max(t.tm_year, 1980)
    return ((year - 1980) << 25 | t.tm_mon << 21 | t.tm_mday << 16 |
            t.tm_hour << 11 | t.tm_min << 5 | t.tm_sec // 2)


OS_ID_AMIGA = ord("A")

EXT_DIRECTORY = 0x02


def entry(name, data, mtime):
    """One level-1 stored header, its extended headers, and its data."""
    directory, _, basename = name.rpartition("/")

    ext = b""
    if directory:
        dirname = (directory.replace("/", "\xff") + "\xff").encode("latin-1")
        block = bytes([EXT_DIRECTORY]) + dirname
        # every extended header ends with the size of the one after it
        ext += struct.pack("<H", len(block) + 2) + block
    ext += struct.pack("<H", 0)    # no (further) extended header

    # The first two bytes of `ext` are the "extended headers size" field that
    # lives at the end of the base header, so the extended headers proper are
    # everything after them.
    ext_bytes = len(ext) - 2

    filename = basename.encode("latin-1")

    body = b"-lh0-"
    body += struct.pack("<I", len(data) + ext_bytes)   # skip size
    body += struct.pack("<I", len(data))               # original size
    body += struct.pack("<I", dos_time(mtime))
    body += bytes([0x20])          # reserved / attribute
    body += bytes([0x01])          # header level 1
    body += bytes([len(filename)]) + filename
    body += struct.pack("<H", crc16(data))
    body += bytes([OS_ID_AMIGA])
    body += ext[:2]                # first extended header's size

    if len(body) > 255:
        raise ValueError(f"name too long for a level 1 header: {name}")

    checksum = sum(body) & 0xFF
    return bytes([len(body), checksum]) + body + ext[2:] + data


def walk(base, members):
    if members:
        for m in members:
            full = os.path.join(base, m)
            if os.path.isdir(full):
                for root, dirs, files in os.walk(full):
                    dirs.sort()
                    for f in sorted(files):
                        p = os.path.join(root, f)
                        yield os.path.relpath(p, base), p
            else:
                yield m, full
        return
    for root, dirs, files in os.walk(base):
        dirs.sort()
        for f in sorted(files):
            p = os.path.join(root, f)
            yield os.path.relpath(p, base), p


def main(argv):
    if len(argv) < 3:
        print(__doc__.strip().splitlines()[0], file=sys.stderr)
        print("usage: lhapack.py <archive.lha> <basedir> [member ...]",
              file=sys.stderr)
        return 2

    archive, base = argv[1], argv[2]
    members = argv[3:]

    total = 0
    count = 0
    with open(archive, "wb") as out:
        for name, path in walk(base, members):
            with open(path, "rb") as fh:
                data = fh.read()
            out.write(entry(name, data, os.path.getmtime(path)))
            total += len(data)
            count += 1
        out.write(b"\0")           # end of archive

    print(f"{archive}: {count} files, {total} bytes stored, "
          f"{os.path.getsize(archive)} bytes of archive")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
