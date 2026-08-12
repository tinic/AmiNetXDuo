#!/usr/bin/env python3
#
# A name server that answers a handful of names and forwards the rest.
#
#   dnspeer.py [-p PORT] [-f UPSTREAM] NAME=A:1.2.3.4,AAAA:2001:db8::1 ...
#
# Written for tests/tools/run-multiaddr.sh, which needs a name whose AAAA
# points somewhere nothing answers and whose A points at a live service.  No
# public name can be made to do that, and DEVS:Internet/hosts cannot hold an
# IPv6 address at all (src/netstack/netstack_dns.c:1043), so the guest is
# pointed at this instead.
#
# Anything not in the table is forwarded to UPSTREAM and the reply relayed
# unchanged, because a static name server goes to the HEAD of the guest's
# list and every other lookup the boot makes would otherwise die here.
#
# SPDX-License-Identifier: MIT

import argparse
import socket
import struct
import sys
import threading

TYPE_A = 1
TYPE_AAAA = 28


def encode_name(name):
    out = b""
    for label in name.rstrip(".").split("."):
        out += bytes([len(label)]) + label.encode("ascii")
    return out + b"\0"


def decode_name(msg, off):
    labels = []
    while True:
        if off >= len(msg):
            raise ValueError("truncated name")
        n = msg[off]
        if n == 0:
            return ".".join(labels), off + 1
        if n & 0xC0:
            raise ValueError("compressed question")
        labels.append(msg[off + 1:off + 1 + n].decode("ascii", "replace"))
        off += 1 + n


class Table:
    def __init__(self, specs):
        self.rows = {}
        for spec in specs:
            name, _, rest = spec.partition("=")
            entry = {}
            for item in rest.split(","):
                kind, _, value = item.partition(":")
                kind = kind.upper()
                if kind == "A":
                    entry[TYPE_A] = socket.inet_pton(socket.AF_INET, value)
                elif kind == "AAAA":
                    entry[TYPE_AAAA] = socket.inet_pton(socket.AF_INET6, value)
                else:
                    raise SystemExit("dnspeer: unknown record type %r" % kind)
            self.rows[name.lower().rstrip(".")] = entry

    def get(self, name):
        return self.rows.get(name.lower().rstrip("."))


def answer(msg, name, qtype, qend, rdata):
    ident, flags, qdcount = struct.unpack("!HHH", msg[0:6])
    header = struct.pack("!HHHHHH", ident, 0x8180, qdcount,
                         1 if rdata else 0, 0, 0)
    body = msg[12:qend]
    if rdata:
        body += encode_name(name) + struct.pack("!HHIH", qtype, 1, 30,
                                                len(rdata)) + rdata
    return header + body


def forward(msg, upstream, timeout=4.0):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(timeout)
    try:
        s.sendto(msg, (upstream, 53))
        reply, _ = s.recvfrom(4096)
        return reply
    except OSError:
        return None
    finally:
        s.close()


def serve(sock, table, upstream, log):
    while True:
        try:
            msg, peer = sock.recvfrom(4096)
        except OSError:
            return
        if len(msg) < 13:
            continue
        try:
            name, off = decode_name(msg, 12)
            qtype, _ = struct.unpack("!HH", msg[off:off + 4])
        except (ValueError, struct.error):
            continue
        qend = off + 4

        row = table.get(name)
        if row is None:
            reply = forward(msg, upstream)
            if reply is not None:
                sock.sendto(reply, peer)
            continue

        rdata = row.get(qtype)
        reply = answer(msg, name, qtype, qend, rdata)
        sock.sendto(reply, peer)
        if log:
            print("%s %s %s -> %s" % (peer[0], name,
                                      {TYPE_A: "A", TYPE_AAAA: "AAAA"}.get(
                                          qtype, qtype),
                                      "answer" if rdata else "no data"),
                  flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-p", "--port", type=int, default=53)
    ap.add_argument("-f", "--forward", default="192.168.1.1")
    ap.add_argument("-q", "--quiet", action="store_true")
    ap.add_argument("map", nargs="+")
    args = ap.parse_args()

    table = Table(args.map)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("0.0.0.0", args.port))
    print("dnspeer: udp/%d, %d names, forwarding to %s"
          % (args.port, len(table.rows), args.forward), flush=True)

    serve(sock, table, args.forward, not args.quiet)


if __name__ == "__main__":
    main()
