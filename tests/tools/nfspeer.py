#!/usr/bin/env python3
"""Portmap, MOUNT and NFSv2 over UDP, enough for one file.

tests/tools/nfsprobe.c is the client.  This answers it, and -- the point of
the whole exercise -- CHECKS THE AUTH_UNIX CREDENTIALS IT IS SENT.  An NFS
server authorises by uid, gid and supplementary groups carried in every
call's RPC header, and nothing else in this tree makes a guest produce them:
they come from usergroup.library, which is the library ch_nfsc could not get
credentials out of in 0.26.5.

Real rpcbind and a real nfsd are not used, and not for convenience: binding
UDP 111 and 2049 needs root, and the question here is what the GUEST sends
and understands, which does not need the kernel's server to answer.  Pass any
ports and tell the probe the same ones.

One line of key=value per interesting event, so the harness can read what
arrived without parsing packets itself.

SPDX-License-Identifier: MIT
"""

import argparse
import socket
import struct
import sys
import time

PMAP_PROG, PMAP_VERS, PMAP_GETPORT = 100000, 2, 3
MNT_PROG,  MNT_VERS,  MNTPROC_MNT  = 100005, 1, 1
NFS_PROG,  NFS_VERS               = 100003, 2
NFSPROC_LOOKUP, NFSPROC_READ      = 4, 6

AUTH_UNIX = 1
FHANDLE = bytes(range(32))          # the one file handle this server has
DIRHANDLE = bytes(range(32, 64))    # and the one directory


def xdr_str(b):
    """XDR opaque/string: length, bytes, pad to 4."""
    pad = (4 - (len(b) % 4)) % 4
    return struct.pack(">I", len(b)) + b + b"\0" * pad


def parse_auth_unix(body):
    """stamp, machinename, uid, gid, gids[] -- the whole reason this exists."""
    stamp = struct.unpack_from(">I", body, 0)[0]
    nlen = struct.unpack_from(">I", body, 4)[0]
    off = 8 + nlen + ((4 - (nlen % 4)) % 4)
    name = body[8:8 + nlen].decode("latin-1", "replace")
    uid, gid, n = struct.unpack_from(">III", body, off)
    off += 12
    gids = list(struct.unpack_from(">" + "I" * n, body, off)) if n else []
    return stamp, name, uid, gid, gids


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=0)
    ap.add_argument("--seconds", type=float, default=120.0)
    ap.add_argument("--content", default="AmiNetXDuo NFS payload, 0123456789\n")
    ap.add_argument("--expect-uid", type=int, default=None)
    ap.add_argument("--expect-gid", type=int, default=None)
    args = ap.parse_args()

    content = args.content.encode("latin-1")

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("0.0.0.0", args.port))
    port = s.getsockname()[1]
    print("nfspeer_port=%d" % port, flush=True)
    print("nfspeer_content_len=%d" % len(content), flush=True)

    deadline = time.time() + args.seconds
    seen = {"getport": 0, "mnt": 0, "lookup": 0, "read": 0, "authunix": 0,
            "authnull": 0, "bad": 0}
    creds_reported = False

    while time.time() < deadline:
        s.settimeout(max(0.2, deadline - time.time()))
        try:
            pkt, peer = s.recvfrom(8192)
        except socket.timeout:
            break
        if len(pkt) < 40:
            seen["bad"] += 1
            continue

        xid, mtype, rpcvers, prog, vers, proc = struct.unpack_from(">IIIIII", pkt, 0)
        if mtype != 0 or rpcvers != 2:
            seen["bad"] += 1
            continue

        off = 24
        cred_flavour, cred_len = struct.unpack_from(">II", pkt, off)
        cred_body = pkt[off + 8:off + 8 + cred_len]
        off += 8 + cred_len + ((4 - (cred_len % 4)) % 4)
        vflav, vlen = struct.unpack_from(">II", pkt, off)
        off += 8 + vlen + ((4 - (vlen % 4)) % 4)

        if cred_flavour == AUTH_UNIX:
            seen["authunix"] += 1
            try:
                stamp, name, uid, gid, gids = parse_auth_unix(cred_body)
            except Exception:
                seen["bad"] += 1
                continue
            if not creds_reported:
                print("cred_flavour=AUTH_UNIX machine=%s uid=%d gid=%d "
                      "ngids=%d gids=%s"
                      % (name, uid, gid, len(gids),
                         ",".join(str(g) for g in gids) or "-"), flush=True)
                creds_reported = True
            if args.expect_uid is not None and uid != args.expect_uid:
                print("cred_uid_mismatch=got:%d,want:%d" % (uid, args.expect_uid),
                      flush=True)
            if args.expect_gid is not None and gid != args.expect_gid:
                print("cred_gid_mismatch=got:%d,want:%d" % (gid, args.expect_gid),
                      flush=True)
        elif cred_flavour == 0:
            seen["authnull"] += 1

        hdr = struct.pack(">IIIIII", xid, 1, 0, 0, 0, 0)  # accepted, AUTH_NULL verf, SUCCESS

        if prog == PMAP_PROG and proc == PMAP_GETPORT:
            wprog, wvers, wproto, _ = struct.unpack_from(">IIII", pkt, off)
            seen["getport"] += 1
            print("getport_for=%d,%d -> %d" % (wprog, wvers, port), flush=True)
            s.sendto(hdr + struct.pack(">I", port), peer)

        elif prog == MNT_PROG and proc == MNTPROC_MNT:
            plen = struct.unpack_from(">I", pkt, off)[0]
            path = pkt[off + 4:off + 4 + plen].decode("latin-1", "replace")
            seen["mnt"] += 1
            print("mnt_path=%s" % path, flush=True)
            s.sendto(hdr + struct.pack(">I", 0) + DIRHANDLE, peer)

        elif prog == NFS_PROG and proc == NFSPROC_LOOKUP:
            nlen = struct.unpack_from(">I", pkt, off + 32)[0]
            name = pkt[off + 36:off + 36 + nlen].decode("latin-1", "replace")
            seen["lookup"] += 1
            print("lookup_name=%s" % name, flush=True)
            fattr = struct.pack(">II", 1, 0o100644) + b"\0" * 60
            s.sendto(hdr + struct.pack(">I", 0) + FHANDLE + fattr, peer)

        elif prog == NFS_PROG and proc == NFSPROC_READ:
            fh = pkt[off:off + 32]
            roff, cnt, _tot = struct.unpack_from(">III", pkt, off + 32)
            seen["read"] += 1
            if fh != FHANDLE:
                print("read_bad_filehandle=1", flush=True)
                s.sendto(hdr + struct.pack(">I", 13), peer)   # NFSERR_ACCES
                continue
            chunk = content[roff:roff + cnt]
            print("read_off=%d count=%d served=%d" % (roff, cnt, len(chunk)),
                  flush=True)
            fattr = struct.pack(">II", 1, 0o100644) + b"\0" * 60
            s.sendto(hdr + struct.pack(">I", 0) + fattr + xdr_str(chunk), peer)

        else:
            seen["bad"] += 1
            print("unhandled_prog=%d proc=%d" % (prog, proc), flush=True)

    print("nfspeer_seen=" + " ".join("%s:%d" % (k, v) for k, v in seen.items()),
          flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
