#!/usr/bin/env python3
"""Read a Fitz soak timeline and say whether anything trended.

    tests/soak/soakreport.py build/testhd-fitzsoak/soak-timeline.csv

Same idea as tests/endurance/endreport.py, and deliberately the same shape of
output, but this timeline has a phase column: a soak that alternates load with
idle wants to know not only whether a counter moved but which phase it moved
in.  A pool that only falls under load and one that falls while nothing is
happening are different bugs, and the per-phase breakdown is what separates
them.

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
    ("est_socks", "sockets in ESTABLISHED"),
]

# Counters that were zero in every trace this project has ever taken.
ZEROS = [
    ("pool_empty_req", "allocations that found the pool empty"),
    ("pool_empty_susp", "threads suspended waiting for a packet"),
    ("tcp_dropped", "TCP connections dropped"),
    ("tcp_rx_drop", "TCP receive drops"),
    ("tcp_cksum", "TCP checksum errors"),
    ("ip_rx_drop", "IP receive drops"),
    ("ip_tx_drop", "IP send drops"),
    ("sana_alloc_fail", "SANA-II allocation failures"),
    ("sana_overrun", "SANA-II overruns"),
    ("sana_rxerr", "SANA-II receive errors"),
    ("sana_txerr", "SANA-II transmit errors"),
    ("errors", "filer I/O errors and corrupt bytes"),
    ("query_fail", "fitz query failures"),
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

    last = rows[-1]
    print("  wire arm:  %s files, %s MB      (Amiga as client, over the A2065)"
          % (last["files_wire"], last["mb_wire"]))
    print("  local arm: %s files, %s MB      (Amiga as server, over 127.0.0.1)"
          % (last["files_local"], last["mb_local"]))
    print("  %s fitz query connections, %s of them failed"
          % (last["queries"], last["query_fail"]))

    moved = int(last["mb_wire"]) + int(last["mb_local"])
    if span > 0:
        print("  mean %.1f KB/s combined" % (moved * 1024.0 / span))

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
        print("    %-24s %10d -> %10d   min %10d at %s%s"
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
        print("    %-40s %d, first at %s" % (label, vals[-1], hms(first)))
    if quiet:
        print("    all zero for the whole run")

    # Retransmissions are not in ZEROS: over SLIRP there is no loss to
    # retransmit for, so a nonzero count here is worth a line of its own
    # rather than a place in a list of things that are expected to be quiet.
    if "tcp_retrans" in rows[0]:
        retx = int(last["tcp_retrans"])
        print()
        print("  TCP retransmissions: %d" % retx)
        if retx:
            first = next(int(r["t_s"]) for r in rows if int(r["tcp_retrans"]))
            print("    first at %s, over SLIRP, which drops nothing, so this"
                  % hms(first))
            print("    is the stack retransmitting against itself")

    # Per-phase drift.  The question the phase engine exists to answer.
    print()
    print("  drift per phase (AvailMem, pool free, live sockets):")
    seen = {}
    for i in range(1, len(rows)):
        ph = rows[i].get("phase", "?")
        if rows[i - 1].get("phase") != ph:
            continue
        d = seen.setdefault(ph, [0, 0, 0, 0])
        d[0] += int(rows[i]["avail_pub"]) - int(rows[i - 1]["avail_pub"])
        d[1] += int(rows[i]["pool_free"]) - int(rows[i - 1]["pool_free"])
        d[2] += int(rows[i]["sockets"]) - int(rows[i - 1]["sockets"])
        d[3] += 1
    for ph in sorted(seen):
        d = seen[ph]
        print("    %-8s avail %+9d   pool %+5d   sockets %+5d   (%d samples)"
              % (ph, d[0], d[1], d[2], d[3]))

    return 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    sys.exit(main(sys.argv[1]))
