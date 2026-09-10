#!/usr/bin/env python3
"""A portmapper's answer, and nothing else.

RpcProbe (tests/tools/rpcprobe.c) sends a real PMAPPROC_GETPORT call; this
answers it.  It exists so the probe can be run without installing rpcbind or
an NFS server on the peer -- the question the probe asks is whether the reply
reaches a bound UDP socket on the guest, and that question does not need a
real portmapper to answer it.

Binding UDP 111 needs root on Linux and this deliberately does not: pass any
port and tell the probe the same one.  The port number is not what is under
test; the source port the GUEST bound is.

One line of key=value per datagram, so the harness can read what arrived.

SPDX-License-Identifier: MIT
"""

import argparse
import socket
import struct
import sys
import time


def be32(b, off):
    return struct.unpack_from(">I", b, off)[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=0,
                    help="UDP port to answer on; 0 asks the kernel")
    ap.add_argument("--seconds", type=float, default=60.0)
    ap.add_argument("--answer-port", type=int, default=2049,
                    help="the port GETPORT reports, NFS by default")
    args = ap.parse_args()

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("0.0.0.0", args.port))
    bound = s.getsockname()[1]
    print("peer_port=%d" % bound, flush=True)

    deadline = time.time() + args.seconds
    served = 0

    while time.time() < deadline:
        s.settimeout(max(0.1, deadline - time.time()))
        try:
            data, addr = s.recvfrom(65535)
        except socket.timeout:
            break

        # A call is at least the header this reads; anything shorter is not one
        # and is reported rather than answered, so a malformed probe is visible.
        if len(data) < 40:
            print("peer_short=%d from=%s:%d" % (len(data), addr[0], addr[1]),
                  flush=True)
            continue

        xid = be32(data, 0)
        mtype = be32(data, 4)
        prog = be32(data, 12)
        proc = be32(data, 20)

        print("peer_call xid=0x%08x msg_type=%d prog=%d proc=%d "
              "from=%s:%d bytes=%d"
              % (xid, mtype, prog, proc, addr[0], addr[1], len(data)),
              flush=True)

        if mtype != 0:
            continue

        # MSG_ACCEPTED / SUCCESS, AUTH_NULL verifier, then the port.
        reply = struct.pack(">IIIIIII",
                            xid,
                            1,      # REPLY
                            0,      # MSG_ACCEPTED
                            0,      # verf flavour AUTH_NULL
                            0,      # verf length
                            0,      # accept_stat SUCCESS
                            args.answer_port)
        s.sendto(reply, addr)
        served += 1
        print("peer_reply xid=0x%08x to=%s:%d port=%d"
              % (xid, addr[0], addr[1], args.answer_port), flush=True)

    print("peer_served=%d" % served, flush=True)
    s.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
