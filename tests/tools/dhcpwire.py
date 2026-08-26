#!/usr/bin/env python3
"""Count DHCP DISCOVERs in a pcap, so a harness can grade the wire.

Prints the count.  -v lists every DHCP message type found instead, which is
what a silent restart needs to show: a file with an ACK and no DISCOVER.

SPDX-License-Identifier: MIT
"""

import struct
import sys

DISCOVER = 1
NAMES = {1: "DISCOVER", 2: "OFFER", 3: "REQUEST", 4: "DECLINE",
         5: "ACK", 6: "NAK", 7: "RELEASE", 8: "INFORM"}


def frames(blob):
    """(link_type, payload) for every record in a classic pcap."""
    if len(blob) < 24:
        return
    magic = blob[:4]
    if magic == b"\xa1\xb2\xc3\xd4":
        end = ">"
    elif magic == b"\xd4\xc3\xb2\xa1":
        end = "<"
    else:
        return
    link = struct.unpack(end + "I", blob[20:24])[0]
    off = 24
    while off + 16 <= len(blob):
        _, _, incl, _ = struct.unpack(end + "IIII", blob[off:off + 16])
        off += 16
        if incl > len(blob) - off:
            return
        yield link, blob[off:off + incl]
        off += incl


def message_type(link, frame):
    """The DHCP option-53 value in this frame, or None."""
    if link != 1 or len(frame) < 14 or frame[12:14] != b"\x08\x00":
        return None
    ip = frame[14:]
    if len(ip) < 20 or (ip[0] >> 4) != 4 or ip[9] != 17:
        return None
    ihl = (ip[0] & 0x0F) * 4
    udp = ip[ihl:]
    if len(udp) < 8:
        return None
    sport, dport = struct.unpack(">HH", udp[0:4])
    if 67 not in (sport, dport) and 68 not in (sport, dport):
        return None
    boot = udp[8:]
    if len(boot) < 240 or boot[236:240] != b"\x63\x82\x53\x63":
        return None
    opt = 240
    while opt < len(boot):
        code = boot[opt]
        if code == 255:
            return None
        if code == 0:
            opt += 1
            continue
        if opt + 2 > len(boot):
            return None
        length = boot[opt + 1]
        if code == 53 and length >= 1 and opt + 2 < len(boot):
            return boot[opt + 2]
        opt += 2 + length
    return None


def main(argv):
    verbose = "-v" in argv
    paths = [a for a in argv[1:] if a != "-v"]
    if not paths:
        print("usage: dhcpwire.py [-v] <file.pcap>", file=sys.stderr)
        return 2

    with open(paths[0], "rb") as handle:
        blob = handle.read()

    seen = []
    for link, frame in frames(blob):
        kind = message_type(link, frame)
        if kind is not None:
            seen.append(kind)

    if verbose:
        if not seen:
            print("no DHCP message in the capture at all")
        for i, kind in enumerate(seen):
            print("%d: %s" % (i + 1, NAMES.get(kind, "type %d" % kind)))
        return 0

    print(sum(1 for kind in seen if kind == DISCOVER))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
