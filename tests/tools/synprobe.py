#!/usr/bin/env python3
"""A legitimate client, timed where it runs.

The oracle for tests/tools/run-synflood.sh and run-syncost.sh: open a real TCP
connection to the guest's httpd, send a GET, read the response, and report how
long the whole thing took.  Repeated, so a single unlucky handshake is not the
measurement.

IT RUNS ON THE PEER, NOT ON THE EMULATOR HOST.  A frame the host running
Amiberry sends to its own bridged guest's MAC never comes back to that NIC's
pcap, so the emulator host gets nothing from its own guest while every other
machine on the LAN reaches it -- docs/RESEARCH.md 63, and the same trap
tests/tools/run-httpd.sh warns about.  Probing from the emulator host does not
measure a slow guest, it measures a bridge that was never going to answer.

The timing is taken here, on the peer, for the same reason a stopwatch goes at
the finish line: driving curl over ssh from a third machine would put an ssh
round trip inside every handshake figure, and an Amiga handshake is smaller
than that.

  synprobe.py HOST --port N --path /small.txt [--count N] [--timeout S]
              [--expect-bytes N]

Output is one key=value line on stdout:

  synprobe: ok=10 total=10 ms_median=7 ms_max=19 bytes=20 bps=0

`bps` is only meaningful with --expect-bytes, where it is the median transfer
rate rather than the handshake time.

Exit 0 when at least one probe completed, 1 when none did, 2 for usage.

SPDX-License-Identifier: MIT
"""

import argparse
import socket
import sys
import time


def one_probe(host, port, path, timeout):
    """Connect, GET, drain.  Returns (milliseconds, bytes) or None."""
    start = time.monotonic()
    sock = None
    try:
        sock = socket.create_connection((host, port), timeout=timeout)
        sock.settimeout(timeout)
        request = ("GET %s HTTP/1.0\r\nHost: %s\r\n"
                   "Connection: close\r\n\r\n" % (path, host))
        sock.sendall(request.encode("ascii"))

        chunks = []
        while True:
            data = sock.recv(65536)
            if not data:
                break
            chunks.append(data)
        body = b"".join(chunks)
    except (OSError, socket.timeout):
        return None
    finally:
        if sock is not None:
            try:
                sock.close()
            except OSError:
                pass

    elapsed_ms = int((time.monotonic() - start) * 1000)

    # Only a 200 counts.  A guest that answers 404 fast is not a guest that
    # served the request.
    if not body.startswith(b"HTTP/1.") or b" 200 " not in body[:64]:
        return None

    head_end = body.find(b"\r\n\r\n")
    payload = len(body) - (head_end + 4) if head_end >= 0 else 0

    return (elapsed_ms, payload)


def median(values):
    if not values:
        return 0
    ordered = sorted(values)
    n = len(ordered)
    if n % 2:
        return ordered[n // 2]
    return (ordered[n // 2 - 1] + ordered[n // 2]) // 2


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host")
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--path", default="/small.txt")
    ap.add_argument("--count", type=int, default=3)
    ap.add_argument("--timeout", type=float, default=8.0)
    ap.add_argument("--gap", type=float, default=1.0)
    ap.add_argument("--expect-bytes", type=int, default=0)
    args = ap.parse_args()

    times = []
    rates = []
    payload_seen = 0
    ok = 0

    for i in range(args.count):
        result = one_probe(args.host, args.port, args.path, args.timeout)
        if result is not None:
            elapsed_ms, payload = result
            if args.expect_bytes and payload < args.expect_bytes:
                # A short read is not a transfer.  Counted as a failure rather
                # than as a very fast one.
                result = None
            else:
                ok += 1
                times.append(elapsed_ms)
                payload_seen = payload
                if elapsed_ms > 0:
                    rates.append(payload * 8000 // elapsed_ms)
        if i + 1 < args.count and args.gap > 0:
            time.sleep(args.gap)

    print("synprobe: ok=%d total=%d ms_median=%d ms_max=%d bytes=%d bps=%d"
          % (ok, args.count, median(times), max(times) if times else 0,
             payload_seen, median(rates)))

    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
