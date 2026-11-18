#!/usr/bin/env python3
"""Broken copies of an update container or manifest, for the scenario tests.

    mkvariant.py member  <in.lha> <out.lha> <member> <file>
        <member>'s data replaced by <file>'s bytes, its CRC-16 and header
        written again: a container that is sound and carries other bytes.
    mkvariant.py flipmember <in.lha> <out.lha> <member>
        one byte in the middle of <member>'s data inverted, CRC-16 written
        again, so only the manifest's per-file SHA-256 can catch it.
    mkvariant.py flipbyte <in> <out>
        one byte in the middle of the file inverted, nothing else touched.
    mkvariant.py manifest <in> <out> <key=value> ...
        container.<flavour>=<file>   that flavour's container= line names
                                     <file>'s size and SHA-256
        file.<flavour>.<path>=<file> that file= line names <file>'s size and
                                     SHA-256
        version=<v>                  the version= line
    mkvariant.py truncate <in> <out>
        the manifest without its last two lines, so end= is missing.

Containers are the level-1 stored archives dist/lhapack.py writes; nothing
else is read.  Output is key=value plus an exit code.

SPDX-License-Identifier: MIT
"""

import hashlib
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "..", "dist"))
import lhapack                                          # noqa: E402


def members(data):
    """(name, mtime, payload) per member of a stored level-1 archive."""
    out = []
    at = 0
    while at < len(data) and data[at] != 0:
        size = data[at]
        body = data[at + 2:at + 2 + size]
        if body[:5] != b"-lh0-" or body[18] != 1:
            raise ValueError("not a stored level-1 member at %d" % at)
        skip, orig, mtime = struct.unpack_from("<III", body, 5)
        nlen = body[19]
        base = body[20:20 + nlen].decode("latin-1")
        ext_next = struct.unpack_from("<H", body, size - 2)[0]
        p = at + 2 + size
        ext_total = 0
        directory = ""
        while ext_next:
            block = data[p:p + ext_next]
            if block[0] == lhapack.EXT_DIRECTORY:
                directory = block[1:ext_next - 2].decode("latin-1") \
                    .replace("\xff", "/")
            ext_total += ext_next
            p += ext_next
            ext_next = struct.unpack_from("<H", block, ext_next - 2)[0]
        payload = data[p:p + skip - ext_total]
        if len(payload) != orig:
            raise ValueError("short member %s" % base)
        out.append((directory + base, mtime, payload))
        at = p + len(payload)
    return out


def dos_to_epoch(stamp):
    import calendar
    year = (stamp >> 25) + 1980
    mon = (stamp >> 21) & 15
    day = (stamp >> 16) & 31
    hour = (stamp >> 11) & 31
    minute = (stamp >> 5) & 63
    sec = (stamp & 31) * 2
    return calendar.timegm((year, mon, day, hour, minute, sec, 0, 0, 0))


def write(path, entries):
    lhapack.FIXED_TIME = 0      # dos_time() then reads the epoch as UTC
    with open(path, "wb") as f:
        for name, mtime, payload in entries:
            f.write(lhapack.entry(name, payload, dos_to_epoch(mtime)))
        f.write(b"\0")


def digest(path):
    with open(path, "rb") as f:
        b = f.read()
    return len(b), hashlib.sha256(b).hexdigest()


def main(argv):
    if len(argv) < 4:
        print(__doc__.strip().splitlines()[0], file=sys.stderr)
        return 2
    op, src, dst = argv[1], argv[2], argv[3]
    with open(src, "rb") as f:
        data = f.read()

    if op in ("member", "flipmember"):
        name = argv[4]
        entries = members(data)
        hit = 0
        for i, (n, mtime, payload) in enumerate(entries):
            if n != name:
                continue
            if op == "member":
                with open(argv[5], "rb") as f:
                    payload = f.read()
            else:
                b = bytearray(payload)
                b[len(b) // 2] ^= 0xFF
                payload = bytes(b)
            entries[i] = (n, mtime, payload)
            hit = 1
        if not hit:
            print("mkvariant=FAIL missing_member=%s" % name)
            return 1
        write(dst, entries)
    elif op == "flipbyte":
        b = bytearray(data)
        b[len(b) // 2] ^= 0xFF
        with open(dst, "wb") as f:
            f.write(b)
    elif op == "truncate":
        lines = data.split(b"\n")
        if lines and lines[-1] == b"":
            lines.pop()
        with open(dst, "wb") as f:
            f.write(b"\n".join(lines[:-2]) + b"\n")
    elif op == "manifest":
        lines = data.decode("ascii").split("\n")
        for arg in argv[4:]:
            key, _, val = arg.partition("=")
            done = 0
            for i, line in enumerate(lines):
                f = line.split(" ")
                if key == "version" and line.startswith("version="):
                    lines[i] = "version=" + val
                    done = 1
                elif key.startswith("container.") and \
                        line.startswith("container=" + key[10:] + " "):
                    size, sha = digest(val)
                    lines[i] = "%s %d %s %s" % (f[0], size, sha, f[3])
                    done = 1
                elif key.startswith("file.") and line.startswith("file="):
                    fl, _, path = key[5:].partition(".")
                    if f[0] == "file=" + fl and f[4] == path:
                        size, sha = digest(val)
                        lines[i] = "%s %s %d %s %s" % (f[0], f[1], size, sha,
                                                       f[4])
                        done = 1
            if not done:
                print("mkvariant=FAIL no_line=%s" % key)
                return 1
        with open(dst, "w") as f:
            f.write("\n".join(lines))
    else:
        print("mkvariant=FAIL op=%s" % op)
        return 2
    size, sha = digest(dst)
    print("mkvariant=PASS op=%s out=%s bytes=%d sha256=%s"
          % (op, os.path.basename(dst), size, sha))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
