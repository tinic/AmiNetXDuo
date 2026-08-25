#!/usr/bin/env python3
"""The other end of a `paysum` run, and the oracle the test asserts against.

This is a second, independent implementation of paysum's pattern and CRC, in a
different language, from the numbers in the comments rather than from the C.
That is the point: src/tools/paysum.c and this file can only agree because the
bytes that crossed the wire are the bytes both ends derived from (seed,
position).  A byte placed at the wrong offset, a word duplicated or skipped, a
chunk leaked in from another connection -- each changes the CRC on exactly one
side of the comparison.

The pattern is position-dependent by construction: word j of a stream is
mix32(seed ^ (j * 0x9E3779B9)), bytes big-endian, where mix32 is the lowbias32
finalizer.  A constant fill (iperf's digits, zeros) would CERTIFY a placement
bug rather than catch it, because bytes landed at the wrong offset still hold
the right values.

  paypeer.py matrix --spec FILE [--bind ADDR] [--lifetime SECONDS]
  paypeer.py selftest

The spec is one case per line, key=value:

  case=3 port=23004 guest=rx len=65537 seed=103 conns=1

`guest=` names the direction from the GUEST's point of view, because the
harness writes both this spec and the guest's command list from one table.
guest=rx: this end accepts and SENDS len patterned bytes.  guest=tx: this end
accepts, receives to EOF, and verifies every byte against the pattern.  With
conns=k the case spans ports port..port+k-1 and seeds seed..seed+k-1, matching
paysum's CONNS argument.

Output is one line per connection and nothing else on stdout:

  paypeer case=3 port=23004 dir=recv bytes=65537 ms=104 crc32=89abcdef \
  sha256=... first_bad=-1 seed=103

and a final `paypeer done cases=N conns=N served=N failed=N`.  Exit 0 when
every expected connection was served and, on the receive side, matched.

SPDX-License-Identifier: MIT
"""

import argparse
import binascii
import hashlib
import socket
import struct
import sys
import threading
import time

try:
    import numpy as _np
except ImportError:          # pure-python fallback, slower but identical
    _np = None

MASK = 0xFFFFFFFF


def mix32(x):
    x &= MASK
    x ^= x >> 16
    x = (x * 0x7FEB352D) & MASK
    x ^= x >> 15
    x = (x * 0x846CA68B) & MASK
    x ^= x >> 16
    return x


def pattern(seed, off, length):
    """Bytes off..off+length-1 of the stream for `seed`, any alignment."""
    if length <= 0:
        return b""
    first_word = off >> 2
    last_word = (off + length - 1) >> 2
    nwords = last_word - first_word + 1

    if _np is not None:
        j = _np.arange(first_word, first_word + nwords, dtype=_np.uint64)
        x = (_np.uint64(seed) ^ (j * _np.uint64(0x9E3779B9))) & _np.uint64(MASK)
        x = x.astype(_np.uint32)
        x ^= x >> _np.uint32(16)
        x = (x * _np.uint32(0x7FEB352D)) & _np.uint32(MASK)
        x ^= x >> _np.uint32(15)
        x = (x * _np.uint32(0x846CA68B)) & _np.uint32(MASK)
        x ^= x >> _np.uint32(16)
        raw = x.byteswap().tobytes()          # big-endian per word
    else:
        words = (mix32(seed ^ ((first_word + k) * 0x9E3779B9))
                 for k in range(nwords))
        raw = b"".join(struct.pack(">I", w) for w in words)

    lead = off & 3
    return raw[lead:lead + length]


def crc32(data, running=0):
    return binascii.crc32(data, running) & MASK


# ------------------------------------------------------------------ serving --

class Case:
    def __init__(self, fields):
        self.case = int(fields["case"])
        self.port = int(fields["port"])
        self.guest = fields["guest"]
        self.len = int(fields["len"])
        self.seed = int(fields["seed"])
        self.conns = int(fields.get("conns", 1))
        if self.guest not in ("rx", "tx"):
            raise ValueError("guest= must be rx or tx")


def emit(line):
    print(line, flush=True)


def serve_conn(conn, case, port, seed, results, idx):
    """One accepted connection: send the pattern or verify it, per the case."""
    ok = False
    t0 = time.time()
    try:
        # A per-operation idle bound, not a transfer budget: a 68020 guest
        # being deliberately starved by the loss arm's blast can spend
        # minutes on one 64 KB case, and a peer that hangs up at sixty
        # seconds turns that into a failure of the PEER's making.
        conn.settimeout(240)
        if case.guest == "rx":
            # The guest receives, so this end sends.  Chunked, so a multi-MB
            # case does not sit in one send() against a 68020 draining it.
            h = hashlib.sha256()
            crc = 0
            sent = 0
            while sent < case.len:
                chunk = pattern(seed, sent, min(65536, case.len - sent))
                conn.sendall(chunk)
                h.update(chunk)
                crc = crc32(chunk, crc)
                sent += len(chunk)
            conn.shutdown(socket.SHUT_WR)
            # The guest's close is its receipt.
            while conn.recv(65536):
                pass
            ms = int((time.time() - t0) * 1000)
            emit("paypeer case=%d port=%d dir=send bytes=%d ms=%d "
                 "crc32=%08x sha256=%s first_bad=-1 seed=%d"
                 % (case.case, port, sent, ms, crc, h.hexdigest(), seed))
            ok = True
        else:
            # The guest sends, so this end receives and verifies.
            h = hashlib.sha256()
            crc = 0
            got = 0
            first_bad = -1
            while True:
                data = conn.recv(65536)
                if not data:
                    break
                h.update(data)
                crc = crc32(data, crc)
                if first_bad < 0:
                    want = pattern(seed, got, len(data))
                    if data != want:
                        for i, (a, b) in enumerate(zip(data, want)):
                            if a != b:
                                first_bad = got + i
                                break
                got += len(data)
            ms = int((time.time() - t0) * 1000)
            emit("paypeer case=%d port=%d dir=recv bytes=%d ms=%d "
                 "crc32=%08x sha256=%s first_bad=%d seed=%d"
                 % (case.case, port, got, ms, crc, h.hexdigest(),
                    first_bad, seed))
            ok = (first_bad < 0) and (got == case.len)
    except OSError as exc:
        print("paypeer: case %d port %d: %s" % (case.case, port, exc),
              file=sys.stderr)
    finally:
        try:
            conn.close()
        except OSError:
            pass
    results[idx] = ok


def listener(bind, case, port, seed, deadline, results, idx):
    ls = socket.socket(socket.AF_INET6, socket.SOCK_STREAM)
    ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    # One socket for both families: v4 callers arrive v4-mapped.
    try:
        ls.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 0)
    except OSError:
        pass
    ls.bind((bind, port))
    ls.listen(1)
    try:
        ls.settimeout(max(1.0, deadline - time.time()))
        conn, _ = ls.accept()
    except socket.timeout:
        print("paypeer: nobody called case %d on port %d" % (case.case, port),
              file=sys.stderr)
        return
    finally:
        pass
    ls.close()
    serve_conn(conn, case, port, seed, results, idx)


def matrix(args):
    cases = []
    with open(args.spec) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            cases.append(Case(dict(kv.split("=", 1) for kv in line.split())))

    deadline = time.time() + args.lifetime
    threads = []
    results = {}
    idx = 0
    for case in cases:
        for k in range(case.conns):
            results[idx] = None
            t = threading.Thread(
                target=listener,
                args=(args.bind, case, case.port + k, case.seed + k,
                      deadline, results, idx),
                daemon=True)
            t.start()
            threads.append(t)
            idx += 1

    for t in threads:
        t.join(max(0.5, deadline + 90 - time.time()))

    served = sum(1 for v in results.values() if v is not None)
    failed = sum(1 for v in results.values() if v is False)
    emit("paypeer done cases=%d conns=%d served=%d failed=%d"
         % (len(cases), len(results), served, failed))
    return 0 if (served == len(results) and failed == 0) else 1


# ----------------------------------------------------------------- selftest --

def selftest(_args):
    """The vectors a reviewer can check by hand, and the alignment edges."""
    rc = 0

    # Slicing must agree with whole-stream generation at every offset.
    whole = pattern(7, 0, 64)
    for off in range(9):
        for ln in range(0, 17):
            if pattern(7, off, ln) != whole[off:off + ln]:
                print("selftest: slice off=%d len=%d disagrees" % (off, ln),
                      file=sys.stderr)
                rc = 1

    # Two seeds must disagree immediately (the cross-connection witness).
    if pattern(1, 0, 16) == pattern(2, 0, 16):
        print("selftest: seeds 1 and 2 agree, the mix is broken",
              file=sys.stderr)
        rc = 1

    # The numpy path and the pure path must be the same function.
    global _np
    if _np is not None:
        np_saved = _np
        a = pattern(99, 3, 4093)
        _np = None
        b = pattern(99, 3, 4093)
        _np = np_saved
        if a != b:
            print("selftest: numpy and pure python disagree", file=sys.stderr)
            rc = 1

    # Pinned vectors, so paysum.c can be checked against the same numbers.
    v = pattern(0, 0, 8)
    emit("selftest seed=0 first8=%s" % v.hex())
    emit("selftest seed=0 len331 crc32=%08x" % crc32(pattern(0, 0, 331)))
    emit("selftest seed=17 len65536 crc32=%08x" % crc32(pattern(17, 0, 65536)))
    emit("selftest %s" % ("FAIL" if rc else "ok"))
    return rc


def main():
    ap = argparse.ArgumentParser(add_help=True)
    sub = ap.add_subparsers(dest="mode", required=True)

    m = sub.add_parser("matrix")
    m.add_argument("--spec", required=True)
    m.add_argument("--bind", default="::")
    m.add_argument("--lifetime", type=float, default=600.0)

    sub.add_parser("selftest")

    args = ap.parse_args()
    if args.mode == "matrix":
        return matrix(args)
    return selftest(args)


if __name__ == "__main__":
    sys.exit(main())
