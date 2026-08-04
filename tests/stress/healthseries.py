#!/usr/bin/env python3
"""
The leak question, as a series rather than as two endpoints.

    tests/stress/healthseries.py DH0:health.log

FitzStress appends a `netstat -h` block every sample, stamped with the second
it was taken.  Two things are being told apart:

  * a LEAK, allocations outstanding or sockets open that do not come back to
    where they started between iterations, while the peaks climb with them;
  * the pool STARVING, `fewest ever` walking down towards zero, which is a
    fixed pool being drained faster than it is returned and is a different
    fault with a different fix.

A count on its own says neither, which is why the peak is printed beside it and
why the whole series is printed rather than the first and last block.

SPDX-License-Identifier: MIT
"""

import re
import sys

PATS = {
    "alloc":    re.compile(r"(\d+) allocations outstanding, (\d+) at the peak, (\d+) refused"),
    "sock":     re.compile(r"(\d+) sockets open, (\d+) at the peak"),
    "pool":     re.compile(r"(\d+) of (\d+) packets free, (\d+) fewest ever"),
    "poolerr":  re.compile(r"(\d+) found the pool empty, (\d+) waited, (\d+) released twice"),
    "mem":      re.compile(r"(\d+) bytes of system memory free, (\d+) in the largest block"),
}


def main():
    if len(sys.argv) < 2:
        sys.stderr.write(__doc__)
        return 2

    try:
        text = open(sys.argv[1], "r", errors="replace").read()
    except OSError as e:
        print("  (no health log: %s)" % e)
        return 0

    rows = []
    cur = None
    for line in text.splitlines():
        m = re.match(r"=====\s*t=(\d+)", line.strip())
        if m:
            if cur:
                rows.append(cur)
            cur = {"t": int(m.group(1))}
            continue
        if cur is None:
            continue
        for key, pat in PATS.items():
            m = pat.search(line)
            if m:
                cur[key] = tuple(int(g) for g in m.groups())
    if cur:
        rows.append(cur)

    rows = [r for r in rows if "alloc" in r]
    if not rows:
        print("  (health log has no readable blocks -- did netstat run?)")
        return 0

    print("     t   alloc  peak  refused   sock  peak    pool  low  empty  "
          "waited     free mem   largest")
    for r in rows:
        a = r.get("alloc", (0, 0, 0))
        s = r.get("sock", (0, 0))
        p = r.get("pool", (0, 0, 0))
        pe = r.get("poolerr", (0, 0, 0))
        m = r.get("mem", (0, 0))
        print("%6d  %6d %5d %8d %6d %5d  %6d %4d %6d %7d %10d %9d"
              % (r["t"], a[0], a[1], a[2], s[0], s[1],
                 p[0], p[2], pe[0], pe[1], m[0], m[1]))

    first, last = rows[0], rows[-1]

    def d(key, i):
        return last.get(key, (0,) * 4)[i] - first.get(key, (0,) * 4)[i]

    print()
    print("  over %d s:" % (last["t"] - first["t"]))
    print("    allocations outstanding %+d (peak %+d), refused %+d"
          % (d("alloc", 0), d("alloc", 1), d("alloc", 2)))
    print("    sockets open            %+d (peak %+d)"
          % (d("sock", 0), d("sock", 1)))
    if "pool" in last:
        low = min(r["pool"][2] for r in rows if "pool" in r)
        tot = last["pool"][1]
        print("    packet pool             %d of %d free at the end, "
              "%d fewest ever" % (last["pool"][0], tot, low))
        print("    pool found empty        %d, waited %d, released twice %d"
              % (last.get("poolerr", (0, 0, 0))[0],
                 last.get("poolerr", (0, 0, 0))[1],
                 last.get("poolerr", (0, 0, 0))[2]))
    print("    free system memory      %+d bytes" % d("mem", 0))

    return 0


if __name__ == "__main__":
    sys.exit(main())
