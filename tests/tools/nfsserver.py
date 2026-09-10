#!/usr/bin/env python3
"""A real NFSv2 + MOUNTv1 server, in userspace, for ch_nfsc to mount.

tests/tools/nfspeer.py answers OUR probe and nothing else.  This serves a real
directory to a real client -- Carsten Heyl's ch_nfsc 1.02BETA, the handler
bifat's reports are about -- so the test is the third-party program doing what
its user does, not our own idea of what it would send.

WHY NOT THE KERNEL'S nfsd.  The peer is a PVE container: `modprobe nfsd` needs
a password this account does not have and nfs-server refuses to start.  What it
does have is a running rpcbind, and rpcbind accepts PMAPPROC_SET from
localhost without privilege -- so this binds unprivileged ports and REGISTERS
them, and a client's GETPORT is answered by the real portmapper with our port.

READ-MOSTLY ON PURPOSE.  MNT, GETATTR, LOOKUP, READ, READDIR and STATFS are
what a mount and a file read need; WRITE and CREATE are here so a failure to
write is a refusal rather than a hang.  Anything else answers NFSERR_ACCES.

Every AUTH_UNIX credential is parsed and logged, because that is the half of
the exchange usergroup.library owns.

SPDX-License-Identifier: MIT
"""

import argparse
import errno
import os
import socket
import stat as statmod
import struct
import sys
import time

PMAP_PORT = 111
PMAP_PROG, PMAP_VERS = 100000, 2
PMAPPROC_SET, PMAPPROC_UNSET = 1, 2
MNT_PROG, MNT_VERS = 100005, 1
NFS_PROG, NFS_VERS = 100003, 2
IPPROTO_UDP = 17

NFS_OK, NFSERR_PERM, NFSERR_NOENT, NFSERR_IO = 0, 1, 2, 5
NFSERR_ACCES, NFSERR_NOTDIR, NFSERR_STALE = 13, 20, 70
NFNON, NFREG, NFDIR = 0, 1, 2
FHSIZE = 32


def pad4(n):
    return (4 - (n % 4)) % 4


def xdr_str(b):
    return struct.pack(">I", len(b)) + b + b"\0" * pad4(len(b))


class Handles:
    """32-byte opaque handles, mapped to paths.  An index and a cookie."""

    def __init__(self):
        self._to_path = {}
        self._to_fh = {}
        self._next = 1

    def fh(self, path):
        path = os.path.realpath(path)
        if path in self._to_fh:
            return self._to_fh[path]
        n = self._next
        self._next += 1
        h = struct.pack(">I", n) + b"\0" * (FHSIZE - 4)
        self._to_fh[path] = h
        self._to_path[h] = path
        return h

    def path(self, h):
        return self._to_path.get(h)


def fattr(path):
    try:
        st = os.lstat(path)
    except OSError:
        return None
    if statmod.S_ISDIR(st.st_mode):
        ftype = NFDIR
    elif statmod.S_ISREG(st.st_mode):
        ftype = NFREG
    else:
        ftype = NFNON
    return struct.pack(
        ">IIIIIIIIIII",
        ftype, st.st_mode & 0xFFFF, st.st_nlink, st.st_uid, st.st_gid,
        st.st_size & 0xFFFFFFFF, 512, 0,
        (st.st_size + 511) // 512, 1, st.st_ino & 0xFFFFFFFF,
    ) + struct.pack(
        ">IIIIII",
        int(st.st_atime), 0, int(st.st_mtime), 0, int(st.st_ctime), 0)


class Server:
    def __init__(self, root, export, quiet=False):
        self.root = os.path.realpath(root)
        self.export = export
        self.h = Handles()
        self.quiet = quiet
        # ONE CREDENTIAL PER PROGRAM, because they are not the same one.  An
        # NFS client sends MNT as root and then the FILE calls as the user it
        # was told to be -- so a single logged credential is always uid 0 and
        # says nothing about whether usergroup.library looked the USER up.
        self.creds_logged = set()
        self.counts = {}

    def log(self, s):
        if not self.quiet:
            print(s, flush=True)

    def bump(self, k):
        self.counts[k] = self.counts.get(k, 0) + 1

    # ---------------------------------------------------------- credentials
    def parse_cred(self, flavour, body, prog):
        if flavour != 1:
            self.bump("auth_null" if flavour == 0 else "auth_other")
            return None
        self.bump("auth_unix")
        try:
            nlen = struct.unpack_from(">I", body, 4)[0]
            off = 8 + nlen + pad4(nlen)
            name = body[8:8 + nlen].decode("latin-1", "replace")
            uid, gid, n = struct.unpack_from(">III", body, off)
            off += 12
            gids = list(struct.unpack_from(">" + "I" * n, body, off)) if n else []
        except Exception:
            self.bump("cred_unparseable")
            return None
        which = {MNT_PROG: "MNT", NFS_PROG: "NFS"}.get(prog, str(prog))
        if which not in self.creds_logged:
            self.log("cred_flavour=AUTH_UNIX prog=%s machine=%s uid=%d gid=%d "
                     "ngids=%d gids=%s" % (which, name, uid, gid, len(gids),
                                          ",".join(str(g) for g in gids) or "-"))
            self.creds_logged.add(which)
        return uid, gid, gids

    # ------------------------------------------------------------ dispatch
    def handle(self, pkt):
        if len(pkt) < 40:
            return None
        xid, mtype, rpcvers, prog, vers, proc = struct.unpack_from(">IIIIII", pkt, 0)
        if mtype != 0 or rpcvers != 2:
            return None
        off = 24
        cf, cl = struct.unpack_from(">II", pkt, off)
        cred = pkt[off + 8:off + 8 + cl]
        off += 8 + cl + pad4(cl)
        vf, vl = struct.unpack_from(">II", pkt, off)
        off += 8 + vl + pad4(vl)
        self.parse_cred(cf, cred, prog)

        ok = struct.pack(">IIIIII", xid, 1, 0, 0, 0, 0)
        args = pkt[off:]

        if prog == MNT_PROG:
            return ok + self.mount_proc(proc, args)
        if prog == NFS_PROG:
            return ok + self.nfs_proc(proc, args)
        self.log("unhandled_prog=%d proc=%d" % (prog, proc))
        return struct.pack(">IIIIIII", xid, 1, 0, 0, 0, 1, 0)   # PROG_UNAVAIL

    # --------------------------------------------------------------- MOUNT
    def mount_proc(self, proc, a):
        if proc == 0:
            return b""
        if proc == 1:                                   # MNT
            plen = struct.unpack_from(">I", a, 0)[0]
            path = a[4:4 + plen].decode("latin-1", "replace")
            self.bump("mnt")
            self.log("mnt_path=%s" % path)
            if path.rstrip("/") not in (self.export.rstrip("/"),):
                self.log("mnt_refused=%s" % path)
                return struct.pack(">I", NFSERR_NOENT)
            return struct.pack(">I", NFS_OK) + self.h.fh(self.root)
        if proc == 3:                                   # UMNT
            self.bump("umnt")
            self.log("umnt=1")
            return b""
        if proc == 5:                                   # EXPORT
            self.bump("export")
            return (struct.pack(">I", 1) + xdr_str(self.export.encode())
                    + struct.pack(">I", 0) + struct.pack(">I", 0))
        return struct.pack(">I", NFSERR_ACCES)

    # ----------------------------------------------------------------- NFS
    def nfs_proc(self, proc, a):
        if proc == 0:
            return b""

        fh = a[:FHSIZE]
        path = self.h.path(fh)
        if path is None and proc not in (0,):
            self.bump("stale")
            return struct.pack(">I", NFSERR_STALE)

        if proc == 1:                                   # GETATTR
            self.bump("getattr")
            at = fattr(path)
            if at is None:
                return struct.pack(">I", NFSERR_NOENT)
            return struct.pack(">I", NFS_OK) + at

        if proc == 2:                                   # SETATTR
            self.bump("setattr")
            at = fattr(path)
            return struct.pack(">I", NFS_OK) + at if at else struct.pack(">I", NFSERR_NOENT)

        if proc == 4:                                   # LOOKUP
            nlen = struct.unpack_from(">I", a, FHSIZE)[0]
            name = a[FHSIZE + 4:FHSIZE + 4 + nlen].decode("latin-1", "replace")
            self.bump("lookup")
            self.log("lookup_name=%s" % name)
            target = os.path.realpath(os.path.join(path, name))
            if not target.startswith(self.root) or not os.path.exists(target):
                return struct.pack(">I", NFSERR_NOENT)
            at = fattr(target)
            return struct.pack(">I", NFS_OK) + self.h.fh(target) + at

        if proc == 6:                                   # READ
            roff, cnt, _tot = struct.unpack_from(">III", a, FHSIZE)
            self.bump("read")
            try:
                with open(path, "rb") as f:
                    f.seek(roff)
                    data = f.read(cnt)
            except OSError:
                return struct.pack(">I", NFSERR_IO)
            self.log("read_off=%d count=%d served=%d" % (roff, cnt, len(data)))
            return struct.pack(">I", NFS_OK) + fattr(path) + xdr_str(data)

        if proc == 8:                                   # WRITE
            _bo, woff, _tc = struct.unpack_from(">III", a, FHSIZE)
            dlen = struct.unpack_from(">I", a, FHSIZE + 12)[0]
            data = a[FHSIZE + 16:FHSIZE + 16 + dlen]
            self.bump("write")
            try:
                with open(path, "r+b") as f:
                    f.seek(woff)
                    f.write(data)
            except OSError:
                return struct.pack(">I", NFSERR_ACCES)
            self.log("write_off=%d len=%d" % (woff, dlen))
            return struct.pack(">I", NFS_OK) + fattr(path)

        if proc == 16:                                  # READDIR
            cookie, count = struct.unpack_from(">II", a, FHSIZE)
            self.bump("readdir")
            try:
                names = sorted(os.listdir(path))
            except OSError:
                return struct.pack(">I", NFSERR_NOTDIR)
            names = [".", ".."] + names
            out = struct.pack(">I", NFS_OK)
            i = cookie
            emitted = 0
            body = b""
            while i < len(names) and len(body) < max(256, count - 128):
                nm = names[i].encode("latin-1", "replace")
                i += 1
                body += (struct.pack(">I", 1) + struct.pack(">I", i)
                         + xdr_str(nm) + struct.pack(">I", i))
                emitted += 1
            body += struct.pack(">I", 0)                     # no more entries
            body += struct.pack(">I", 1 if i >= len(names) else 0)   # eof
            self.log("readdir_cookie=%d emitted=%d eof=%d"
                     % (cookie, emitted, 1 if i >= len(names) else 0))
            return out + body

        if proc == 17:                                  # STATFS
            self.bump("statfs")
            try:
                vfs = os.statvfs(path)
                blocks, bfree, bavail = vfs.f_blocks, vfs.f_bfree, vfs.f_bavail
                bsize = vfs.f_bsize
            except OSError:
                blocks = bfree = bavail = 0
                bsize = 512
            return struct.pack(">IIIIII", NFS_OK, 8192, bsize,
                               blocks & 0x7FFFFFFF, bfree & 0x7FFFFFFF,
                               bavail & 0x7FFFFFFF)

        self.bump("unhandled_nfs_%d" % proc)
        self.log("unhandled_nfs_proc=%d" % proc)
        return struct.pack(">I", NFSERR_ACCES)


def pmap(proc, prog, vers, port):
    """PMAPPROC_SET / UNSET against the local rpcbind."""
    m = struct.pack(">IIIIII", 0x5A5A0000 + proc, 0, 2, PMAP_PROG, PMAP_VERS, proc)
    m += struct.pack(">II", 0, 0) + struct.pack(">II", 0, 0)
    m += struct.pack(">IIII", prog, vers, IPPROTO_UDP, port)
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(3)
    try:
        s.sendto(m, ("127.0.0.1", PMAP_PORT))
        r, _ = s.recvfrom(4096)
        vlen = struct.unpack_from(">I", r, 16)[0]
        off = 20 + vlen + pad4(vlen)
        if struct.unpack_from(">I", r, off)[0] != 0:
            return False
        return struct.unpack_from(">I", r, off + 4)[0] == 1
    except Exception:
        return False
    finally:
        s.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True, help="directory to serve")
    ap.add_argument("--export", default="/export", help="the exported path name")
    ap.add_argument("--port", type=int, default=0)
    ap.add_argument("--seconds", type=float, default=300.0)
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("0.0.0.0", args.port))
    port = s.getsockname()[1]

    srv = Server(args.root, args.export, args.quiet)
    print("nfsserver_port=%d root=%s export=%s" % (port, srv.root, srv.export),
          flush=True)

    reg = []
    for prog, vers in ((MNT_PROG, MNT_VERS), (NFS_PROG, NFS_VERS)):
        # UNSET FIRST, ALWAYS.  rpcbind refuses a SET for a prog/vers that is
        # already registered, so a previous run whose UNSET did not happen --
        # killed by a signal, or its `finally` skipped -- makes every later run
        # fail to register and look like a broken server.  Unsetting an entry
        # that is not there is harmless.
        pmap(PMAPPROC_UNSET, prog, vers, port)
        if pmap(PMAPPROC_SET, prog, vers, port):
            reg.append("%d/%d" % (prog, vers))
        else:
            print("rpcbind_register_FAILED prog=%d vers=%d" % (prog, vers),
                  flush=True)
    print("rpcbind_registered=%s" % (",".join(reg) or "none"), flush=True)

    deadline = time.time() + args.seconds
    try:
        while time.time() < deadline:
            s.settimeout(max(0.2, deadline - time.time()))
            try:
                pkt, peer = s.recvfrom(65536)
            except socket.timeout:
                break
            try:
                rep = srv.handle(pkt)
            except Exception as e:                       # never die on a client
                print("server_exception=%r" % (e,), flush=True)
                continue
            if rep is not None:
                s.sendto(rep, peer)
    finally:
        for prog, vers in ((MNT_PROG, MNT_VERS), (NFS_PROG, NFS_VERS)):
            pmap(PMAPPROC_UNSET, prog, vers, port)
        print("nfsserver_counts=" +
              " ".join("%s:%d" % (k, v) for k, v in sorted(srv.counts.items())),
              flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
