#!/usr/bin/env python3
# The name server tests/tools/run-dnsguard.sh points the guest at.
# SPDX-License-Identifier: MIT

import argparse
import socket
import struct
import sys

TYPE_A = 1
TYPE_NS = 2
TYPE_CNAME = 5
TYPE_SOA = 6
TYPE_PTR = 12
TYPE_AAAA = 28
CLASS_IN = 1

RCODE_NOERROR = 0
RCODE_NXDOMAIN = 3

TYPE_NAMES = {TYPE_A: "A", TYPE_NS: "NS", TYPE_CNAME: "CNAME",
              TYPE_SOA: "SOA", TYPE_PTR: "PTR", TYPE_AAAA: "AAAA"}


def encode_name(name):
    out = b""
    for label in name.rstrip(".").split("."):
        raw = label.encode("ascii")
        if not 1 <= len(raw) <= 63:
            raise ValueError("bad label %r" % label)
        out += bytes([len(raw)]) + raw
    return out + b"\x00"


def decode_name(msg, off):
    labels = []
    hops = 0
    while True:
        if off >= len(msg):
            raise ValueError("name runs off the end")
        length = msg[off]
        if length == 0:
            off += 1
            break
        if length & 0xC0 == 0xC0:
            target = ((length & 0x3F) << 8) | msg[off + 1]
            off += 2
            hops += 1
            if hops > 8:
                raise ValueError("compression pointer loop")
            tail, _ = decode_name(msg, target)
            labels.append(tail)
            return ".".join(labels), off
        off += 1
        labels.append(msg[off:off + length].decode("ascii", "replace"))
        off += length
    return ".".join(labels), off


def rr(owner, rtype, ttl, rdata):
    return (encode_name(owner) +
            struct.pack("!HHIH", rtype, CLASS_IN, ttl, len(rdata)) + rdata)


def a_rdata(text):
    return socket.inet_aton(text)


def soa_rdata(zone, minimum):
    return (encode_name("ns." + zone) + encode_name("hostmaster." + zone) +
            struct.pack("!IIIII", 1, 3600, 900, 604800, minimum))


class Zone(object):

    def __init__(self, zone, negative_ttl, ttl):
        self.zone = zone.rstrip(".").lower()
        self.negative_ttl = negative_ttl
        self.ttl = ttl

    def _in_zone(self, name):
        return name == self.zone or name.endswith("." + self.zone)

    def holds(self, qname):
        name = qname.rstrip(".").lower()
        return self._in_zone(name) or name.endswith(".in-addr.arpa")

    def answer(self, qname, qtype):
        """Returns (rcode, answers, authority)."""
        name = qname.rstrip(".").lower()

        if name.endswith(".in-addr.arpa"):
            arpa_soa = [rr("in-addr.arpa", TYPE_SOA, self.ttl,
                           soa_rdata(self.zone, self.negative_ttl))]
            if qtype == TYPE_PTR and name == "1.0.0.10.in-addr.arpa":
                return RCODE_NOERROR, [rr(name, TYPE_PTR, self.ttl,
                                          encode_name("plain." + self.zone))], []
            return RCODE_NXDOMAIN, [], arpa_soa

        if not self._in_zone(name):
            return RCODE_NXDOMAIN, [], [rr(self.zone, TYPE_SOA, self.ttl,
                                           soa_rdata(self.zone,
                                                     self.negative_ttl))]

        label = name[:-(len(self.zone) + 1)] if name != self.zone else ""

        if label == "plain":
            if qtype == TYPE_A:
                return RCODE_NOERROR, [rr(name, TYPE_A, self.ttl,
                                          a_rdata("10.0.0.1"))], []
            return RCODE_NOERROR, [], []

        if label == "target":
            if qtype == TYPE_A:
                return RCODE_NOERROR, [rr(name, TYPE_A, self.ttl,
                                          a_rdata("10.0.0.2"))], []
            return RCODE_NOERROR, [], []

        if label == "alias":
            if qtype == TYPE_A:
                target = "target." + self.zone
                return RCODE_NOERROR, [
                    rr(name, TYPE_CNAME, self.ttl, encode_name(target)),
                    rr(target, TYPE_A, self.ttl, a_rdata("10.0.0.2")),
                ], []
            return RCODE_NOERROR, [], []

        if label == "evil":
            if qtype == TYPE_A:
                return RCODE_NOERROR, [rr("attacker.example", TYPE_A,
                                          self.ttl, a_rdata("10.6.6.6"))], []
            return RCODE_NOERROR, [], []

        return RCODE_NXDOMAIN, [], [rr(self.zone, TYPE_SOA, self.ttl,
                                       soa_rdata(self.zone,
                                                 self.negative_ttl))]


def forward(request, upstream, timeout=4.0):
    """The reply a real resolver gives, verbatim.  None if it does not."""
    relay = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    relay.settimeout(timeout)
    try:
        relay.sendto(request, (upstream, 53))
        reply, _ = relay.recvfrom(4096)
        return reply
    except OSError:
        return None
    finally:
        relay.close()


def serve(sock, zone, upstream):
    while True:
        try:
            request, peer = sock.recvfrom(4096)
        except KeyboardInterrupt:
            return
        except OSError:
            return

        if len(request) < 12:
            continue

        ident, flags, qdcount = struct.unpack("!HHH", request[:6])
        if qdcount != 1:
            continue

        try:
            qname, off = decode_name(request, 12)
            qtype, qclass = struct.unpack("!HH", request[off:off + 4])
        except (ValueError, struct.error):
            continue

        outside = upstream and not zone.holds(qname)

        sys.stdout.write("Q %s %s %s%s\n" % (qname.lower(),
                                             TYPE_NAMES.get(qtype, str(qtype)),
                                             peer[0],
                                             " FWD" if outside else ""))
        sys.stdout.flush()

        if qclass != CLASS_IN:
            continue

        if outside:
            reply = forward(request, upstream)
            if reply is not None:
                sock.sendto(reply, peer)
            continue

        rcode, answers, authority = zone.answer(qname, qtype)

        header = struct.pack("!HHHHHH", ident, 0x8400 | rcode, 1,
                             len(answers), len(authority), 0)
        body = request[12:off + 4] + b"".join(answers) + b"".join(authority)

        sock.sendto(header + body, peer)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--zone", default="dnsguard.test")
    parser.add_argument("--bind", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=53)
    parser.add_argument("--negative-ttl", type=int, default=600)
    parser.add_argument("--ttl", type=int, default=600)
    parser.add_argument("--forward", default="")
    args = parser.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((args.bind, args.port))

    sys.stdout.write("READY %s:%d zone=%s negative-ttl=%d forward=%s\n" %
                     (args.bind, args.port, args.zone, args.negative_ttl,
                      args.forward or "none"))
    sys.stdout.flush()

    serve(sock, Zone(args.zone, args.negative_ttl, args.ttl), args.forward)


if __name__ == "__main__":
    main()
