#!/usr/bin/env python3
"""Read an endurance timeline and say whether anything trended.

    tests/endurance/endreport.py build/testhd-fitz/end-timeline.csv

A multi-hour run produces a few hundred rows and the question asked of them is
never "what was the value" but "did it move".  A leak, a fragmenting heap and a
packet pool that never comes back are all *trends*, and all three look like
noise in any single row, docs/RESEARCH.md 24.9 already records one 291 KB
AvailMem drop that was a library loading and not a leak.

So this prints, for every column that can only be read as a trend, the first
value, the last value, the minimum, and where the minimum was.  Counters that
should be zero for the whole run are called out separately, because for those
the interesting number is "did it ever move at all".

SPDX-License-Identifier: MIT
"""

import sys
import csv


# Columns whose LEVEL matters: a floor reached once is the result.
LEVELS = [
    ("pool_free", "packet pool free"),
    ("avail_pub", "AvailMem(PUBLIC)"),
    ("avail_largest", "largest free block"),
    ("sockets", "live sockets"),
]

# Counters that were zero in every trace this project has ever taken (16.4,
# 24.4).  Any of them moving is the news.
ZEROS = [
    ("pool_empty_req", "allocations that found the pool empty"),
    ("pool_empty_susp", "threads suspended waiting for a packet"),
    ("pool_bad_release", "invalid packet releases"),
    ("tcp_retx", "TCP retransmissions"),
    ("tcp_rxdrop", "TCP receive drops"),
    ("tcp_cksum", "TCP checksum errors"),
    ("tcp_drop", "TCP connections dropped"),
    ("ip_rxdrop", "IP receive drops"),
    ("ip_txdrop", "IP send drops"),
    ("if_allocfail", "SANA-II allocation failures"),
    ("if_overrun", "SANA-II overruns"),
    ("if_rxerr", "SANA-II receive errors"),
    ("if_txerr", "SANA-II transmit errors"),
]


def hms(seconds):
    seconds = int(seconds)
    return "%d:%02d:%02d" % (seconds // 3600, (seconds // 60) % 60, seconds % 60)


def main(path):
    with open(path, newline="") as fh:
        rows = list(csv.DictReader(fh))

    rows = [r for r in rows if r.get("t_s")]
    if not rows:
        print("  (timeline is empty)")
        return 1

    span = int(rows[-1]["t_s"])
    print("  %d samples over %s" % (len(rows), hms(span)))

    tx = int(rows[-1]["tx_mb"])
    rx = int(rows[-1]["rx_mb"])
    print("  moved %d MB out, %d MB in, %s transactions, %s connections"
          % (tx, rx, rows[-1]["xacts"], rows[-1]["conns"]))

    if span > 0:
        print("  mean %.1f KB/s combined"
              % ((tx + rx) * 1024.0 / span))

    # A 32-bit sequence space is 4096 MB.  Say plainly whether the run got
    # there, because "does a long connection survive wrap" cannot be answered
    # by a run that never wrapped one.
    print("  sequence-space progress: %d MB of the 4096 MB a 32-bit sequence "
          "number covers" % max(tx, rx))

    print()
    print("  levels (first -> last, and the worst point):")
    for key, label in LEVELS:
        if key not in rows[0]:
            continue
        vals = [int(r[key]) for r in rows]
        lo = min(vals)
        at = int(rows[vals.index(lo)]["t_s"])
        drift = vals[-1] - vals[0]
        flag = ""
        if key in ("avail_pub", "avail_largest") and drift < -65536:
            flag = "   <-- LOST %d bytes over the run" % (-drift)
        if key == "pool_free" and lo == 0:
            flag = "   <-- REACHED ZERO"
        print("    %-22s %10d -> %10d   min %10d at %s%s"
              % (label, vals[0], vals[-1], lo, hms(at), flag))

    print()
    print("  counters that should stay at zero:")
    quiet = True
    for key, label in ZEROS:
        if key not in rows[0]:
            continue
        vals = [int(r[key]) for r in rows]
        if vals[-1] == 0:
            continue
        quiet = False
        first = next(int(r["t_s"]) for r, v in zip(rows, vals) if v)
        print("    %-38s %d, first at %s" % (label, vals[-1], hms(first)))
    if quiet:
        print("    all zero for the whole run")

    return 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    sys.exit(main(sys.argv[1]))
