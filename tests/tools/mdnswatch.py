#!/usr/bin/env python3
"""Listen for mDNS on the host, so a run can say whether SLIRP relays it.

    tests/tools/mdnswatch.py --log FILE --seconds N [--respond NAME=ADDR]

WHY THIS EXISTS

    The guest's own frames are recoverable without any help: the emulated A2065
    dumps every frame it handles into fs-uae.log.txt and tests/trace/a2065pcap.py
    turns that into a pcap (docs/RESEARCH.md 16.3).  That proves what LEFT the
    Amiga.  It cannot prove what FS-UAE's SLIRP then did with it, because SLIRP
    is a user-mode NAT inside the emulator process and its output goes straight
    to the host's sockets without crossing an interface anything can watch.

    So the only way to find out whether a multicast datagram from the guest
    reaches the host's real network is to sit on the host's real network and
    wait.  That is all this is: a socket bound to UDP 5353, joined to
    224.0.0.251 on every interface it can find, writing down what arrives.

    A NEGATIVE RESULT IS THE POINT.  If nothing arrives, that is the measured
    answer to "does SLIRP pass multicast", and it is worth just as much as a
    positive one -- provided the instrument is known to work, which is what
    --selftest is for.

--respond, and why it is optional

    mDNS resolution needs somebody to answer.  With --respond NAME=ADDR this
    process will answer an A query for NAME.local with ADDR, which turns it
    into the minimal peer the guest could resolve against IF the relay works.
    Left off, it only ever listens, and never puts a packet on the network of
    whoever is running the test.

macOS

    Nearly every host already runs a responder (mDNSResponder, or avahi-daemon
    on Linux) bound to 5353.  SO_REUSEPORT/SO_REUSEADDR is what makes a second
    listener legal alongside it; without both, bind() fails with EADDRINUSE and
    this says so rather than pretending the network was quiet.

SPDX-License-Identifier: MIT
"""

import argparse
import errno
import os
import socket
import struct
import sys
import time

MCAST_ADDR = "224.0.0.251"
MCAST_PORT = 5353


def log_open(path):
    if not path:
        return sys.stdout
    return open(path, "w", buffering=1)


def parse_name(data, offset, depth=0):
    """Decode a DNS name. Returns (labels, next_offset)."""
    labels = []
    if depth > 8:
        return labels, offset
    while offset < len(data):
        n = data[offset]
        if n == 0:
            return labels, offset + 1
        if n & 0xC0 == 0xC0:
            if offset + 1 >= len(data):
                return labels, len(data)
            ptr = ((n & 0x3F) << 8) | data[offset + 1]
            more, _ = parse_name(data, ptr, depth + 1)
            labels.extend(more)
            return labels, offset + 2
        offset += 1
        labels.append(data[offset:offset + n].decode("utf-8", "replace"))
        offset += n
    return labels, offset


RRTYPE = {1: "A", 12: "PTR", 16: "TXT", 28: "AAAA", 33: "SRV", 47: "NSEC"}


def rrname(rrtype):
    return RRTYPE.get(rrtype, "type %d" % rrtype)


def summarise(data):
    """One line describing an mDNS message, without a full parser.

    The answer section is walked as well as the question section: an
    announcement carries no questions at all, so a summary that stopped at the
    questions would print "response an=8" and nothing about WHAT was announced
    -- which is the whole thing a service test wants to read.
    """
    if len(data) < 12:
        return "short packet (%d bytes)" % len(data)
    xid, flags, qd, an, ns, ar = struct.unpack(">HHHHHH", data[:12])
    kind = "response" if flags & 0x8000 else "query"
    names = []
    offset = 12
    for _ in range(min(qd, 8)):
        labels, offset = parse_name(data, offset)
        if offset + 4 > len(data):
            break
        qtype, _qclass = struct.unpack(">HH", data[offset:offset + 4])
        offset += 4
        names.append("%s(%s)" % (".".join(labels), rrname(qtype)))

    # Answers, authority and additional all share the resource-record layout,
    # so one walk covers them; rdata is skipped by its own length.
    for _ in range(min(an + ns + ar, 16)):
        labels, offset = parse_name(data, offset)
        if offset + 10 > len(data):
            break
        rrtype, _cls, _ttl, rdlen = struct.unpack(">HHIH",
                                                  data[offset:offset + 10])
        offset += 10 + rdlen
        if offset > len(data):
            break
        names.append("%s=%s" % (rrname(rrtype), ".".join(labels)))

    return "%s id=0x%04x qd=%d an=%d ns=%d ar=%d %s" % (
        kind, xid, qd, an, ns, ar, " ".join(names))


def build_response(name_labels, addr, xid):
    """A minimal mDNS response carrying one A record, cache-flush set."""
    out = struct.pack(">HHHHHH", xid, 0x8400, 0, 1, 0, 0)
    body = b""
    for label in name_labels:
        body += bytes([len(label)]) + label.encode()
    body += b"\x00"
    # type A, class IN with the RFC 6762 10.2 cache-flush bit, TTL 120.
    body += struct.pack(">HHIH", 1, 0x8001, 120, 4)
    body += socket.inet_aton(addr)
    return out + body


def join_groups(sock, log):
    """Join 224.0.0.251 on every interface we can name. Returns how many."""
    joined = 0
    addrs = {"0.0.0.0"}
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None,
                                       socket.AF_INET):
            addrs.add(info[4][0])
    except socket.gaierror:
        pass

    for addr in sorted(addrs):
        try:
            mreq = socket.inet_aton(MCAST_ADDR) + socket.inet_aton(addr)
            sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)
            joined += 1
            log.write("joined %s on %s\n" % (MCAST_ADDR, addr))
        except OSError as exc:
            if exc.errno not in (errno.EADDRINUSE, errno.EADDRNOTAVAIL):
                log.write("join on %s failed: %s\n" % (addr, exc))
    return joined


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--log", default=None)
    ap.add_argument("--seconds", type=float, default=300.0)
    ap.add_argument("--respond", default=None,
                    metavar="NAME=ADDR",
                    help="answer A queries for NAME.local with ADDR")
    ap.add_argument("--selftest", action="store_true",
                    help="send one query to 224.0.0.251 and require to see it")
    args = ap.parse_args()

    log = log_open(args.log)

    respond_name = respond_addr = None
    if args.respond:
        respond_name, _, respond_addr = args.respond.partition("=")
        respond_name = respond_name.lower()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    if hasattr(socket, "SO_REUSEPORT"):
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
    try:
        sock.bind(("", MCAST_PORT))
    except OSError as exc:
        log.write("CANNOT BIND UDP %d: %s\n" % (MCAST_PORT, exc))
        log.write("INSTRUMENT UNAVAILABLE -- no conclusion can be drawn\n")
        log.flush()
        return 2

    joined = join_groups(sock, log)
    log.write("listening on UDP %d, %d group membership(s), pid %d\n"
              % (MCAST_PORT, joined, os.getpid()))
    if respond_name:
        log.write("will answer A queries for %s.local with %s\n"
                  % (respond_name, respond_addr))
    log.flush()

    if args.selftest:
        probe = struct.pack(">HHHHHH", 0x4d44, 0x0000, 1, 0, 0, 0)
        probe += b"\x09mdnswatch\x05local\x00" + struct.pack(">HH", 1, 1)
        sock.sendto(probe, (MCAST_ADDR, MCAST_PORT))
        log.write("selftest: sent a query for mdnswatch.local\n")
        log.flush()

    deadline = time.time() + args.seconds
    seen = 0
    selftest_ok = False

    while time.time() < deadline:
        sock.settimeout(max(0.2, min(2.0, deadline - time.time())))
        try:
            data, peer = sock.recvfrom(9000)
        except socket.timeout:
            continue
        except OSError as exc:
            log.write("recv failed: %s\n" % exc)
            break

        seen += 1
        text = summarise(data)
        log.write("[%8.2f] %s:%d  %s\n"
                  % (time.time() % 100000, peer[0], peer[1], text))
        if "mdnswatch.local" in text:
            selftest_ok = True

        if respond_name and len(data) >= 12:
            flags = struct.unpack(">H", data[2:4])[0]
            if not flags & 0x8000:
                labels, _ = parse_name(data, 12)
                if [l.lower() for l in labels] == [respond_name, "local"]:
                    xid = struct.unpack(">H", data[:2])[0]
                    reply = build_response(labels, respond_addr, xid)
                    sock.sendto(reply, (MCAST_ADDR, MCAST_PORT))
                    log.write("           answered %s.local -> %s (multicast)\n"
                              % (respond_name, respond_addr))
                    #
                    # And again, UNICAST straight back to where it came from.
                    #
                    # RFC 6762 6.7 already requires this of any responder when
                    # the query's source port is not 5353 -- and a query that
                    # crossed FS-UAE's SLIRP arrives with a rewritten source
                    # port, so it always looks like a legacy unicast query from
                    # out here.  Sending it deliberately is how the NAT's
                    # return path gets measured rather than guessed: if the
                    # guest's A2065 log shows this frame arriving, inbound
                    # works and only the multicast direction is lost; if it
                    # shows nothing, the NAT binding is one-way.
                    #
                    if peer[1] != MCAST_PORT:
                        sock.sendto(reply, peer)
                        log.write("           and unicast to %s:%d\n"
                                  % (peer[0], peer[1]))
        log.flush()

    log.write("--- %d mDNS message(s) seen in %.0f s ---\n"
              % (seen, args.seconds))
    if args.selftest:
        log.write("selftest: the instrument %s its own multicast\n"
                  % ("SAW" if selftest_ok else "DID NOT SEE"))
    log.flush()
    return 0


if __name__ == "__main__":
    sys.exit(main())
