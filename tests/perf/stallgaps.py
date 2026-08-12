#!/usr/bin/env python3
"""Where a transfer stopped, and for how long, from a peer-side capture.

    tests/perf/stallgaps.py peer.pcap [--port 17712] [--dir read]

WHY NOT A MEAN
--------------------------------------------------------------------------
A user reporting "it hangs and restarts" is describing a stall long enough
to notice.  A mean throughput figure cannot show one: 4 MB in 60 seconds
reads the same whether the bytes arrived evenly or in two bursts either side
of a ten-second freeze.  tests/perf/lossrate.py answers what fraction of
segments were retransmitted; this answers when nothing moved.

WHAT IT MEASURES
--------------------------------------------------------------------------
Forward progress, not packets.  A retransmission carries no new bytes, so a
capture full of them is not progress and a gap measured between frames would
miss the stall entirely.  The clock here advances only when the highest
sequence number the peer has put on the wire does.

    stall = seconds between one new byte and the next

Reported as a distribution -- the per-interval byte counts, the quantiles of
the gaps, and every gap over --report -- rather than a single number,
because the shape is the finding: proportional loss of throughput and a
multi-second freeze are the same mean and different defects.

READ AND WRITE STAY SEPARATE, for the reason spelled out at the top of
tests/perf/run-lossgate.sh: the 0.16.6 regression moved them opposite ways.
--dir picks one; the phases come from lossrate.py, so they are the same
phases that file's numbers are about.

WHERE THE TIMESTAMPS COME FROM
--------------------------------------------------------------------------
The capture is taken at the peer, so a peer -> guest frame is stamped as it
was SENT and a guest -> peer frame as it ARRIVED.  On a run with netem delay
towards the guest that makes the sender's gaps exact and the acknowledgement
side late by one delay -- which is what is wanted, since the question is when
the sender stopped having anything it was allowed to send.

SPDX-License-Identifier: MIT
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from lossrate import Windows, phases, read_capture, seq_lt   # noqa: E402


def progress(segs, port, span, peer_is_sender):
    """[(t, new_bytes)] over one phase, counting only bytes never sent before.

    Retransmissions and pure acknowledgements contribute nothing.  The result
    is the arrival of NEW data in time, which is what a user sees stop.
    """
    t0, t1 = span[0], span[1]
    out = []
    high = None
    for s in segs:
        if s.t < t0 or s.t > t1 or s.plen == 0:
            continue
        if (s.sport == port) != peer_is_sender:
            continue
        end = (s.seq + s.plen) & 0xFFFFFFFF
        if high is None:
            out.append((s.t, s.plen))
            high = end
            continue
        if seq_lt(high, end):
            new = (end - high) & 0xFFFFFFFF
            out.append((s.t, new))
            high = end
    return out


def quantile(sorted_values, q):
    if not sorted_values:
        return 0.0
    i = int(q * (len(sorted_values) - 1))
    return sorted_values[i]


def main():
    ap = argparse.ArgumentParser(
        description="when a transfer stopped making forward progress, and "
                    "for how long, from a peer-side capture")
    ap.add_argument("pcap")
    ap.add_argument("--port", type=int, default=17712)
    ap.add_argument("--eph", type=int)
    ap.add_argument("--dir", choices=("read", "write"), default="read",
                    help="read is peer -> guest, which is the field metric")
    ap.add_argument("--bulk", type=int, default=512)
    ap.add_argument("--gap", type=float, default=0.25,
                    help="seconds of quiet that end a phase (default 0.25)")
    ap.add_argument("--min-phase", type=int, default=1 << 18)
    ap.add_argument("--report", type=float, default=0.5,
                    help="print every gap at or above this many seconds "
                         "(default 0.5)")
    ap.add_argument("--interval", type=float, default=0.1,
                    help="bucket width for the progress histogram in seconds "
                         "(default 0.1)")
    ap.add_argument("--buckets", action="store_true",
                    help="print every interval's byte count, for plotting")
    ap.add_argument("--brief", action="store_true",
                    help="one key=value line")
    args = ap.parse_args()

    port = args.port
    key, segs, _n = read_capture(args.pcap, port, args.eph)
    if not port:
        port = min(key)
    spans = phases(segs, port, args.bulk, args.gap, args.min_phase)
    want = args.dir == "read"
    mine = [s for s in spans if s[2] == want]
    if not mine:
        raise SystemExit("%s: no %s phase in this capture" %
                         (args.pcap, args.dir))

    gaps = []
    total_bytes = 0
    total_span = 0.0
    lines = []
    buckets = []
    for span in mine:
        prog = progress(segs, port, span, want)
        if len(prog) < 2:
            continue
        total_span += prog[-1][0] - prog[0][0]
        for i in range(1, len(prog)):
            dt = prog[i][0] - prog[i - 1][0]
            total_bytes += prog[i][1]
            gaps.append(dt)
            if dt >= args.report:
                lines.append((dt, prog[i - 1][0] - prog[0][0], prog[i][1]))
        if args.buckets:
            base = prog[0][0]
            for t, n in prog:
                b = int((t - base) / args.interval)
                while len(buckets) <= b:
                    buckets.append(0)
                buckets[b] += n

    gaps.sort()
    longest = gaps[-1] if gaps else 0.0
    over = {x: sum(1 for g in gaps if g >= x) for x in (0.2, 0.5, 1.0, 2.0, 5.0)}
    # Seconds spent inside a gap at or over half a second: the share of the
    # transfer a user would call a freeze rather than slowness.
    stalled = sum(g for g in gaps if g >= args.report)
    rate = total_bytes / total_span / 1024.0 if total_span > 0 else 0.0

    if args.brief:
        print("dir=%s kbs=%.1f span_s=%.2f longest_gap_ms=%.0f "
              "p99_gap_ms=%.1f stalled_s=%.2f stalled_pct=%.1f "
              "ge200ms=%d ge500ms=%d ge1s=%d ge2s=%d ge5s=%d" % (
                  args.dir, rate, total_span, longest * 1000,
                  quantile(gaps, 0.99) * 1000, stalled,
                  100.0 * stalled / total_span if total_span else 0.0,
                  over[0.2], over[0.5], over[1.0], over[2.0], over[5.0]))
        return 0

    print("%s: %s phase(s), %s direction" % (args.pcap, len(mine), args.dir))
    print("  new bytes      %d over %.2f s = %.1f KB/s" %
          (total_bytes, total_span, rate))
    print("  gaps between one new byte and the next, ms:")
    print("    p50 %.2f  p90 %.2f  p99 %.2f  p999 %.2f  max %.0f" % (
        quantile(gaps, 0.5) * 1000, quantile(gaps, 0.9) * 1000,
        quantile(gaps, 0.99) * 1000, quantile(gaps, 0.999) * 1000,
        longest * 1000))
    print("    >=200ms %d  >=500ms %d  >=1s %d  >=2s %d  >=5s %d" % (
        over[0.2], over[0.5], over[1.0], over[2.0], over[5.0]))
    print("  time inside a gap >= %.1fs: %.2f s, %.1f%% of the transfer" % (
        args.report, stalled,
        100.0 * stalled / total_span if total_span else 0.0))
    if lines:
        print("  every gap >= %.1fs (offset into the phase):" % args.report)
        for dt, at, n in sorted(lines, reverse=True)[:40]:
            print("    %8.0f ms at t+%7.2f s, then %d new bytes" %
                  (dt * 1000, at, n))
    if args.buckets:
        print("  bytes per %.0f ms:" % (args.interval * 1000))
        for i, n in enumerate(buckets):
            print("    %8.2f %d" % (i * args.interval, n))
    return 0


if __name__ == "__main__":
    sys.exit(main())
