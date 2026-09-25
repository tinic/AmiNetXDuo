#!/usr/bin/env python3
"""Send one mDNS query for a name from OUTSIDE the guest, and print the reply.

The foreign-multicast half of the #38 demonstration: a process on the host
(not the guest, not a loopback path) puts a query for the guest's own name on
224.0.0.251:5353.  The guest's built-in mDNS responder answers it, and a
co-bound BSD 5353 listener records the same datagram.  This script proves the
first half (the responder received and answered); the guest transcript proves
the second.

The query is sent from an ephemeral source port, so the responder's reply comes
back UNICAST to this socket (RFC 6762 6.7), and the script does not have to
join the group to hear it.

    tests/tools/mcastquery.py NAME [--repeat N] [--wait SECONDS]

Prints exactly one verdict line per query, and a final summary line.

SPDX-License-Identifier: MIT
"""

import argparse
import fcntl
import socket
import struct
import sys
import time

MCAST_ADDR = "224.0.0.251"
MCAST_PORT = 5353


def iface_ipv4(name):
    """The IPv4 address of a host interface, or None."""
    try:
        ifreq = struct.pack("256s", name.encode()[:15])
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        addr = fcntl.ioctl(sock.fileno(), 0x8915, ifreq)[20:24]  # SIOCGIFADDR
        sock.close()
        return socket.inet_ntoa(addr)
    except OSError:
        return None


def build_query(name):
    """A minimal mDNS query: one question, type A, class IN."""
    labels = name.rstrip(".").split(".")
    out = struct.pack(">HHHHHH", 0x4D44, 0x0000, 1, 0, 0, 0)
    for label in labels:
        raw = label.encode("ascii")
        out += bytes([len(raw)]) + raw
    out += b"\x00"
    out += struct.pack(">HH", 1, 0x0001)  # A, IN (no cache-flush in a query)
    return out


def summarise(data):
    """One short line describing what came back."""
    if len(data) < 12:
        return "short (%d bytes)" % len(data)
    _xid, flags, qd, an, ns, ar = struct.unpack(">HHHHHH", data[:12])
    kind = "response" if flags & 0x8000 else "query"
    return "%s qd=%d an=%d ns=%d ar=%d" % (kind, qd, an, ns, ar)


def once(name, wait, iface=None):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    s.settimeout(wait)
    if iface:
        addr = iface_ipv4(iface)
        if addr:
            # IP_MULTICAST_IF (9) = the in_addr of the egress interface.
            s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF,
                         socket.inet_aton(addr))
    q = build_query(name)
    s.sendto(q, (MCAST_ADDR, MCAST_PORT))
    sent = time.time()
    try:
        data, peer = s.recvfrom(9000)
        print("REPLY from %s:%d, %d bytes, %s (%.2fs)"
              % (peer[0], peer[1], len(data), summarise(data),
                 time.time() - sent))
        return True
    except socket.timeout:
        print("NO REPLY for %s in %.0fs" % (name, wait))
        return False
    finally:
        s.close()


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("name")
    ap.add_argument("--repeat", type=int, default=1)
    ap.add_argument("--wait", type=float, default=4.0)
    ap.add_argument("--iface", default=None)
    args = ap.parse_args(argv)

    ok = 0
    for _ in range(args.repeat):
        if once(args.name, args.wait, args.iface):
            ok += 1
        time.sleep(1.0)

    print("mcastquery: %d/%d reply(s) for %s" % (ok, args.repeat, args.name))
    return 0 if ok > 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
