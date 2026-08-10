#!/usr/bin/env python3
"""The other end of an `iperf` run, and the oracle the test asserts against.

This is a second, independent implementation of the same wire format that
src/tools/iperfwire.c writes, in a different language, from the same capture of
iperf 2.2.1.  That is the point: a test where the guest and the peer share an
encoder can only prove the two agree with each other.  What it reports is what
actually crossed the wire on this side, counted by the kernel's own recv().

It is iperf 2, on port 5001.  iperf 3 is a different protocol on port 5201.

  iperfpeer.py serve tcp  [--port N] [--seconds N]
  iperfpeer.py serve udp  [--port N] [--seconds N]
  iperfpeer.py send  tcp  HOST [--port N] [--seconds N] [--length N]
  iperfpeer.py send  udp  HOST [--port N] [--seconds N] [--length N] [--kbit N]

Output is one key=value line per run on stdout and nothing else on it, so a
caller may parse it without knowing anything about the prose:

  peer_role=serve peer_proto=tcp peer_bytes=4194304 peer_ms=10012 \
  peer_bits_per_sec=3351000 peer_packets=1024 peer_lost=0 peer_outoforder=0

Exit code 0 when the run completed, 1 when it did not, 2 for usage.

SPDX-License-Identifier: MIT
"""

import argparse
import socket
import struct
import sys
import time

HEADER_VERSION1 = 0x80000000

DG_LEN = 12
CLIENT_HDR = 24
DG_TOTAL = DG_LEN + CLIENT_HDR
REPORT_OFF = 16
REPORT_LEN = 128

DEFAULT_PORT = 5001
DEFAULT_UDP_LEN = 1470
DEFAULT_TCP_LEN = 4096


def pattern(length, offset=0):
    """iperf's own payload, the digits repeating.

    The first four bytes are read by an iperf 2 receiver as a client_hdr and
    it switches mode on the top two bits.  '0123' is 0x30313233, which has
    both clear, so a stream that starts here is taken as plain data.
    """
    return bytes((0x30 + ((offset + i) % 10)) for i in range(length))


def dg_put(buf, ident, now):
    """The datagram header, and a zeroed client_hdr after it.

    The zeros are load-bearing.  Left as whatever the buffer held, the flags
    word lands on the server's client_hdr and 0x20000000 in it puts iperf 2.2
    into trip-time mode, after which it reports one datagram and drops the
    rest of the run.
    """
    head = struct.pack(">iII", ident, int(now), int((now % 1) * 1e6))
    return head + b"\0" * CLIENT_HDR + buf[DG_TOTAL:]


def dg_id(data):
    return struct.unpack(">i", data[:4])[0]


def report_put(total, ms, lost, outoforder, datagrams):
    body = struct.pack(">I", HEADER_VERSION1)
    body += struct.pack(">II", total >> 32, total & 0xFFFFFFFF)
    body += struct.pack(">II", ms // 1000, (ms % 1000) * 1000)
    body += struct.pack(">III", lost, outoforder, datagrams)
    body += struct.pack(">II", 0, 0)
    out = b"\0" * REPORT_OFF + body
    return out + b"\0" * (REPORT_LEN - len(out))


def report_get(data):
    if len(data) < REPORT_OFF + 40:
        return None
    fields = struct.unpack(">10I", data[REPORT_OFF:REPORT_OFF + 40])
    if not fields[0] & HEADER_VERSION1:
        return None
    return {
        "flags": fields[0],
        "bytes": (fields[1] << 32) | fields[2],
        "ms": fields[3] * 1000 + fields[4] // 1000,
        "lost": fields[5],
        "outoforder": fields[6],
        "datagrams": fields[7],
    }


def bits_per_sec(total, ms):
    if ms <= 0:
        return 0
    return (total * 8000 + ms // 2) // ms


def emit(role, proto, total, ms, packets, lost, outoforder, extra=""):
    line = ("peer_role=%s peer_proto=%s peer_bytes=%d peer_ms=%d "
            "peer_bits_per_sec=%d peer_packets=%d peer_lost=%d "
            "peer_outoforder=%d"
            % (role, proto, total, ms, bits_per_sec(total, ms), packets,
               lost, outoforder))
    if extra:
        line += " " + extra
    print(line, flush=True)


# ------------------------------------------------------------------ serving --

def serve_tcp(args):
    ls = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ls.bind((args.bind, args.port))
    ls.listen(4)

    if args.keep:
        # One line per caller, until the lifetime runs out.  The leak sweep
        # runs the same command several times against one peer, and a peer
        # that served once and exited would make every repetition after the
        # first fail as "connection refused".
        deadline = time.time() + args.seconds
        served = 0
        while time.time() < deadline:
            ls.settimeout(max(0.5, deadline - time.time()))
            try:
                conn, _ = ls.accept()
            except socket.timeout:
                break
            if serve_tcp_conn(conn, args) == 0:
                served += 1
        ls.close()
        return 0 if served else 1

    ls.settimeout(args.seconds)
    try:
        conn, _ = ls.accept()
    except socket.timeout:
        print("iperfpeer: nobody connected to tcp/%d within %d seconds"
              % (args.port, args.seconds), file=sys.stderr)
        return 1
    rc = serve_tcp_conn(conn, args)
    ls.close()
    return rc


def serve_tcp_conn(conn, args):
    conn.settimeout(args.idle)
    total = 0
    packets = 0
    t0 = None
    last = None
    while True:
        try:
            data = conn.recv(65536)
        except socket.timeout:
            break
        if not data:
            break
        if t0 is None:
            t0 = time.time()
        total += len(data)
        packets += 1
        last = time.time()
    conn.close()

    if t0 is None:
        print("iperfpeer: the connection carried no bytes", file=sys.stderr)
        return 1

    emit("serve", "tcp", total, int((last - t0) * 1000), packets, 0, 0)
    return 0


def serve_udp(args):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((args.bind, args.port))

    if args.keep:
        # The socket is bound once and the run state is reset after each end
        # marker, so caller after caller is served on the same port.
        deadline = time.time() + args.seconds
        served = 0
        while time.time() < deadline:
            if serve_udp_once(s, args, max(0.5, deadline - time.time())) == 0:
                served += 1
            else:
                break
        s.close()
        return 0 if served else 1

    rc = serve_udp_once(s, args, args.seconds)
    s.close()
    return rc


def serve_udp_once(s, args, wait):
    s.settimeout(wait)

    total = 0
    packets = 0
    lost = 0
    outoforder = 0
    expect = 1
    t0 = None
    peer = None
    while True:
        try:
            data, peer = s.recvfrom(65536)
        except socket.timeout:
            if t0 is None:
                print("iperfpeer: no datagram reached udp/%d within %d seconds"
                      % (args.port, args.seconds), file=sys.stderr)
                return 1
            break
        if t0 is None:
            t0 = time.time()
        total += len(data)
        packets += 1

        ident = dg_id(data) if len(data) >= 4 else 0
        if ident < 0:
            ms = int((time.time() - t0) * 1000)
            ack = report_put(total, ms, lost, outoforder, packets - 1)
            # Repeated because it is a datagram and the sender is listening
            # for exactly one; iperf 2 does the same.
            for _ in range(10):
                s.sendto(ack, peer)
                time.sleep(0.02)
            emit("serve", "udp", total, ms, packets - 1, lost, outoforder)
            return 0
        if ident == expect:
            expect += 1
        elif ident > expect:
            lost += ident - expect
            expect = ident + 1
        else:
            outoforder += 1

    # No end marker.  Still a measurement, and saying so is better than
    # failing: a sender that was killed still moved these bytes.
    emit("serve", "udp", total, int((time.time() - t0) * 1000), packets,
         lost, outoforder, "peer_fin=0")
    return 0


# ------------------------------------------------------------------ sending --

def send_tcp(args):
    buf = pattern(args.length)
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(args.idle)
    try:
        s.connect((args.host, args.port))
    except OSError as exc:
        print("iperfpeer: cannot connect to %s:%d: %s"
              % (args.host, args.port, exc), file=sys.stderr)
        return 1

    total = 0
    packets = 0
    t0 = time.time()
    try:
        while time.time() - t0 < args.seconds:
            n = s.send(buf)
            total += n
            packets += 1
    except OSError as exc:
        print("iperfpeer: send failed after %d bytes: %s" % (total, exc),
              file=sys.stderr)
        s.close()
        return 1

    ms = int((time.time() - t0) * 1000)
    s.shutdown(socket.SHUT_WR)
    s.close()
    emit("send", "tcp", total, ms, packets, 0, 0)
    return 0


def send_udp(args):
    buf = pattern(args.length)
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(0.25)
    s.connect((args.host, args.port))

    total = 0
    seq = 0
    t0 = time.time()
    while True:
        now = time.time()
        if now - t0 >= args.seconds:
            break
        if args.kbit:
            allowed = args.kbit * (now - t0) * 1000 / 8
            if total >= allowed:
                time.sleep(0.001)
                continue
        seq += 1
        total += s.send(dg_put(buf, seq, now))

    ms = int((time.time() - t0) * 1000)

    got = None
    for _ in range(10):
        s.send(dg_put(buf, -seq, time.time()))
        try:
            ack = s.recv(65536)
        except socket.timeout:
            continue
        got = report_get(ack)
        if got:
            break

    s.close()
    if got is None:
        emit("send", "udp", total, ms, seq, 0, 0, "peer_report=0")
        return 0

    emit("send", "udp", total, ms, seq, got["lost"], got["outoforder"],
         "peer_report=1 peer_far_bytes=%d peer_far_ms=%d peer_far_datagrams=%d"
         % (got["bytes"], got["ms"], got["datagrams"]))
    return 0


def main():
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("role", choices=("serve", "send"))
    ap.add_argument("proto", choices=("tcp", "udp"))
    ap.add_argument("host", nargs="?")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--seconds", type=float, default=10.0)
    ap.add_argument("--length", type=int, default=0)
    ap.add_argument("--kbit", type=int, default=0)
    ap.add_argument("--idle", type=float, default=5.0,
                    help="give up after this long with nothing arriving")
    ap.add_argument("--keep", action="store_true",
                    help="serve caller after caller until --seconds runs out")
    args = ap.parse_args()

    if args.length == 0:
        args.length = DEFAULT_UDP_LEN if args.proto == "udp" \
            else DEFAULT_TCP_LEN

    if args.role == "send" and not args.host:
        ap.error("send needs a host")

    if args.role == "serve":
        return serve_tcp(args) if args.proto == "tcp" else serve_udp(args)
    return send_tcp(args) if args.proto == "tcp" else send_udp(args)


if __name__ == "__main__":
    sys.exit(main())
