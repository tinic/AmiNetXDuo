#!/usr/bin/env python3
"""Read tickprobe's samples and say what the stack's periodic timer runs at.

    tests/compare/tickphase.py build/tickprobe-tick-ours.txt [more...]

WHAT IT DOES, AND WHY IT IS NOT A HISTOGRAM OF ONE GAP

A frame a stack sends because a timer fired leaves on that timer's grid.  If
the grid spacing is T0 then every INTERVAL between two such frames is an
integer multiple of T0, whatever the timer's own period is and whoever set it.
So for a candidate T,

    R(T) = | mean_k exp(2*pi*i * gap_k / T) |

is near 1 when every gap is a whole number of T and near 0 otherwise, and
R sweeps out a comb with a tooth at T0 and at every T0/m.  The tick is the
LARGEST T with a tooth: quantisation at 10 ms implies quantisation at 5 ms,
and quantisation at 20 ms implies it at 10 ms, which is exactly why "the gaps
are multiples of 10 ms" on its own settles nothing.

Intervals rather than absolute times on purpose.  Absolute phase coherence
over a minute-long run would need the tick source to be exact to a part in
10^6; a 400 ms interval tolerates a hundred times worse.

THE CONTROL IS REPORTED WITH THE RESULT.  The harness's own injections were
spaced by a pseudo-random UNIT_MICROHZ sleep, so their intervals must show NO
tooth anywhere.  If they do, the harness was aliasing against the system tick
and the result line is meaningless, so both are printed, always.

SPDX-License-Identifier: MIT
"""

import math
import sys


def parse(path):
    """One tickprobe run: eclock rate, and every sample keyed by kind."""
    run = {
        "path": path,
        "eclock_hz": None,
        "icmp": [],       # (seq, payload_len, t0, t1, arp)
        "dack": [],       # (i, t0, t1, win)
        "rtx": [],        # (n, t, flags, seq)
        "notes": [],
        "problems": [],
        "timebase": {},   # name -> (rounds, each_us, ticks)
    }

    with open(path, "r", errors="replace") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            if line.startswith("!!"):
                run["problems"].append(line)
                continue
            if line.startswith("#"):
                run["notes"].append(line)
                if "eclock_hz=" in line:
                    run["eclock_hz"] = int(line.split("eclock_hz=")[1].split()[0])
                if line.startswith("# timebase "):
                    f = line.split()
                    kv = dict(t.split("=") for t in f[3:] if "=" in t)
                    run["timebase"][f[2]] = (int(kv["rounds"]),
                                             int(kv.get("each", 0)),
                                             int(kv["t1"]) - int(kv["t0"]))
                continue

            head, _, rest = line.partition(" ")
            kv = {}
            for tok in rest.split():
                k, _, v = tok.partition("=")
                kv[k] = v

            if head == "icmp" and "t0" in kv and "t1" in kv:
                run["icmp"].append((int(kv["seq"]), int(kv["len"]),
                                    int(kv["t0"]), int(kv["t1"]),
                                    int(kv.get("arp", 0))))
            elif head == "dack" and "t0" in kv and "t1" in kv:
                run["dack"].append((int(kv["i"]), int(kv["t0"]), int(kv["t1"]),
                                    int(kv.get("win", 0))))
            elif head == "rtx" and "t" in kv:
                run["rtx"].append((int(kv["n"]), int(kv["t"]),
                                   int(kv.get("flags", "0"), 16),
                                   int(kv.get("seq", 0))))
            elif head in ("icmp", "dack"):
                run["problems"].append(line)

    return run


def unwrap(values):
    """E-Clock is 32 bits and wraps every 6055 s; a run is shorter than that."""
    out = []
    add = 0
    prev = None
    for v in values:
        if prev is not None and v < prev - (1 << 31):
            add += 1 << 32
        out.append(v + add)
        prev = v
    return out


def stats(xs):
    if not xs:
        return None
    s = sorted(xs)
    n = len(s)

    def q(p):
        return s[min(n - 1, int(p * n))]

    mean = sum(s) / n
    var = sum((x - mean) ** 2 for x in s) / n
    return {
        "n": n, "min": s[0], "p10": q(0.10), "p50": q(0.50),
        "p90": q(0.90), "max": s[-1], "mean": mean, "sd": math.sqrt(var),
    }


try:
    import numpy as _np
except ImportError:                                     # pragma: no cover
    _np = None


def comb(gaps, t_lo, t_hi, step):
    """R(T) over a range of candidate grid spacings.  gaps in seconds."""
    n = len(gaps)
    if n == 0:
        return []

    if _np is not None:
        ts = _np.arange(t_lo, t_hi + step / 2, step)
        a = 2.0 * _np.pi * (_np.asarray(gaps)[None, :] / ts[:, None])
        r = _np.abs(_np.cos(a).sum(1) + 1j * _np.sin(a).sum(1)) / n
        return list(zip(ts.tolist(), r.tolist()))

    out = []
    t = t_lo
    while t <= t_hi:
        re = im = 0.0
        for g in gaps:
            a = 2.0 * math.pi * (g / t)
            re += math.cos(a)
            im += math.sin(a)
        out.append((t, math.hypot(re, im) / n))
        t += step
    return out


def teeth(curve, floor=0.80):
    """Local maxima of R above `floor`, largest T first."""
    found = []
    for i in range(1, len(curve) - 1):
        t, r = curve[i]
        if r >= floor and r >= curve[i - 1][1] and r >= curve[i + 1][1]:
            if found and abs(found[-1][0] - t) < t * 0.01:
                if r > found[-1][1]:
                    found[-1] = (t, r)
                continue
            found.append((t, r))
    return sorted(found, key=lambda tr: -tr[0])


def grid_report(name, times_s, lo_ms=1.0, hi_ms=260.0):
    """The whole periodogram argument for one population of event times."""
    if len(times_s) < 8:
        print(f"  {name}: only {len(times_s)} events -- not enough to say anything")
        return None

    gaps = [b - a for a, b in zip(times_s, times_s[1:])]
    gaps = [g for g in gaps if g > 0]

    # Coarse pass, then a fine pass around every candidate.  A tooth at 20 ms
    # measured over 400 ms gaps is about 0.25 ms wide, so 5 us is plenty
    # coarse and 10 ns resolves the peak.
    coarse = comb(gaps, lo_ms / 1000.0, hi_ms / 1000.0, 5e-6)
    cand = teeth(coarse, 0.75)

    refined = []
    for t, _ in cand[:10]:
        fine = comb(gaps, max(1e-4, t - 1e-5), t + 1e-5, 1e-8)
        tt, rr = max(fine, key=lambda x: x[1])
        refined.append((tt, rr))
    refined.sort(key=lambda x: -x[0])

    spread = max(gaps) - min(gaps)
    print(f"  {name}: {len(gaps)} intervals, "
          f"{min(gaps) * 1000:.1f}..{max(gaps) * 1000:.1f} ms "
          f"(spread {spread * 1000:.0f} ms)")
    if not refined:
        print("    no grid: R(T) < 0.75 everywhere in "
              f"{lo_ms:g}..{hi_ms:g} ms -- these events are not quantised")
        return None

    # Ranked by R, not by T.  A comb has a tooth at the grid AND at every
    # submultiple of it, and it also has weaker teeth at multiples when most
    # but not all of the intervals happen to be multiples of those too, so
    # "the largest T with a tooth" is the wrong rule and the strongest one,
    # with the harmonic ratios printed next to it, is the right one.
    best = max(refined, key=lambda tr: tr[1])
    print("    T (ms)      R      T / strongest")
    for t, r in sorted(refined, key=lambda tr: -tr[1])[:8]:
        print(f"    {t * 1000:9.4f}  {r:.4f}   {t / best[0]:7.4f}")
    print(f"    -> strongest grid: {best[0] * 1000:.4f} ms  "
          f"({1.0 / best[0]:.3f} Hz), R = {best[1]:.4f}")
    if spread < 3 * best[0]:
        print("    !! the intervals span fewer than three grid steps: nearly "
              "equal intervals are\n       congruent modulo almost anything, "
              "so this fit is not evidence")

    # A grid coarser than any plausible tick means the events are released by
    # a protocol timer with its own period, and the tick is that period over
    # some integer this measurement cannot pin down on its own.  Print the
    # candidates rather than pick one.
    if best[0] > 0.030:
        print("    this is a protocol timer's own period, not the tick: only")
        print("    MULTIPLES of the tick are visible in it.  The divisors "
              "that land in the")
        print("    range an Amiga periodic timer can plausibly occupy:")
        for n in range(1, 60):
            t = best[0] / n
            if 0.008 <= t <= 0.025:
                print(f"      /{n:<3d} = {t * 1000:8.4f} ms  "
                      f"({1.0 / t:7.3f} Hz)")

    # And the two hypotheses named in the question, whatever the fit says.
    for t in (0.010, 0.020):
        r = comb(gaps, t, t, 1.0)[0][1]
        print(f"    R({t * 1000:g} ms exactly) = {r:.3f}")
    return best


def report(run):
    """Print everything one run has to say, and return its ACK grid."""
    best = None
    hz = run["eclock_hz"] or 709379
    print("=" * 72)
    print(run["path"])
    print(f"  E-Clock {hz} Hz")
    for p in run["problems"][:10]:
        print(f"  !! {p}")

    if run["timebase"]:
        print("\n  What the machine offers, timed by the harness itself")
        for name, (rounds, each, ticks) in run["timebase"].items():
            per = ticks / hz / rounds * 1000.0
            extra = f", asked for {each / 1000.0:g} ms" if each else ""
            print(f"    {name:8s} {rounds} rounds -> {per:.5f} ms each "
                  f"({1000.0 / per:.4f} Hz{extra})")

    def to_s(ticks):
        return ticks / hz

    # ---------------------------------------------------------- ICMP ------
    by_len = {}
    for _seq, ln, t0, t1, arp in run["icmp"]:
        if arp:
            continue
        by_len.setdefault(ln, []).append((t1 - t0) / hz * 1e6)

    if by_len:
        print("\n  ICMP echo turnaround -- arrival-driven, no timer involved")
        for ln in sorted(by_len):
            s = stats(by_len[ln])
            print(f"    payload {ln:5d} B, n={s['n']:3d}   "
                  f"min {s['min'] / 1000:6.3f}  p50 {s['p50'] / 1000:6.3f}  "
                  f"p90 {s['p90'] / 1000:6.3f}  max {s['max'] / 1000:6.3f} ms")
        if len(by_len) >= 2:
            a, b = sorted(by_len)
            sa, sb = stats(by_len[a]), stats(by_len[b])
            dbytes = b - a
            dus = sb["p50"] - sa["p50"]
            print(f"    per-byte from the two sizes: "
                  f"{dus / dbytes:.3f} us/B over {dbytes} B "
                  f"({dus / 1000:.3f} ms), fixed cost "
                  f"{(sa['p50'] - dus / dbytes * a) / 1000:.3f} ms")

    # ---------------------------------------------------- delayed ACK -----
    if run["dack"]:
        t0 = unwrap([d[1] for d in run["dack"]])
        t1 = unwrap([d[2] for d in run["dack"]])
        delays = [(b - a) / hz * 1e3 for a, b in zip(t0, t1)]

        # A stack that acknowledges every second segment answers half of these
        # at once and holds the other half for its timer, and the two are
        # different measurements: one is the receive path, one is the tick.
        # Split them where the delays themselves say to, not at a number
        # somebody picked.
        order = sorted(range(len(delays)), key=lambda i: delays[i])
        sd = [delays[i] for i in order]
        cut, cutsize = None, 0.0
        for i in range(1, len(sd)):
            if sd[i] - sd[i - 1] > cutsize:
                cutsize, cut = sd[i] - sd[i - 1], i
        two = (cut is not None and cutsize > 0.25 * (sd[-1] - sd[0]) and
               cut >= 8 and len(sd) - cut >= 8)

        if two:
            thresh = (sd[cut] + sd[cut - 1]) / 2.0
            prompt = [i for i in range(len(delays)) if delays[i] < thresh]
            held = [i for i in range(len(delays)) if delays[i] >= thresh]
            p = stats([delays[i] for i in prompt])
            print("\n  ACKs answered at once -- arrival-driven, one TCP data "
                  "segment in and one ACK out")
            print(f"    n={p['n']}  min {p['min']:.2f}  p50 {p['p50']:.2f}  "
                  f"p90 {p['p90']:.2f}  max {p['max']:.2f} ms")
        else:
            thresh = None
            held = list(range(len(delays)))
            print("\n  Every ACK was held: no segment was answered promptly")

        h = stats([delays[i] for i in held])
        print("\n  ACKs held for a timer -- timer-driven")
        print(f"    n={h['n']}  min {h['min']:.2f}  p50 {h['p50']:.2f}  "
              f"p90 {h['p90']:.2f}  max {h['max']:.2f} ms")
        # A delay counted down in whole ticks from an arrival that lands at a
        # uniformly random phase is uniform over ONE tick, so the width of the
        # delay distribution reads the tick period off directly, and it does
        # so without any of the periodogram's assumptions.
        print(f"    spread {h['max'] - h['min']:.2f} ms  <- one tick, if the "
              "hold is a whole number of ticks")

        print("\n  Grid of the held ACKs' departures")
        best = grid_report("held acks", [t1[i] / hz for i in held])
        print("\n  Control: the harness's own injections, which were "
              "deliberately unquantised")
        ctl = grid_report("injections", [t / hz for t in t0])
        if ctl is not None:
            print("    !! the CONTROL is quantised too: the harness was "
                  "aliasing against\n       something, and the result above "
                  "is not evidence of anything")

        if best:
            per = best[0] * 1000.0
            base = t1[held[0]] / hz * 1000.0
            resid = []
            for i in held:
                x = (t1[i] / hz * 1000.0 - base) / per
                resid.append(abs(x - round(x)) * per * 1000.0)
            r = stats(resid)
            print(f"\n    departures off the {per:.4f} ms grid: "
                  f"p50 {r['p50']:.0f} us  p90 {r['p90']:.0f} us  "
                  f"max {r['max']:.0f} us")

    # ------------------------------------------------------ retransmit ----
    if run["rtx"]:
        ts = unwrap([r[1] for r in run["rtx"]])
        gaps = [(b - a) / hz * 1e3 for a, b in zip(ts, ts[1:])]
        print("\n  Retransmission of an unanswered SYN")
        print(f"    {len(ts)} frames, gaps: " +
              ", ".join(f"{g:.1f}" for g in gaps) + " ms")
        print("    the first gap runs from an application instant to a timer "
              "instant and is")
        print("    NOT a whole number of ticks; only gaps between two "
              "timer-driven frames are")

    return best


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2

    grids = []
    for path in argv[1:]:
        g = report(parse(path))
        if g:
            grids.append((path, g[0]))

    # The cross-check that turns two separate fits into one statement.  A
    # protocol timer's period is a whole number of ticks; if one stack's grid
    # is an exact integer multiple of another's, they are counting the same
    # thing, and if it is not, they are not.
    if len(grids) >= 2:
        print("=" * 72)
        print("Grids against each other -- a protocol period is a whole "
              "number of ticks")
        for i, (pa, ga) in enumerate(grids):
            for pb, gb in grids[i + 1:]:
                lo, hi = sorted((ga, gb))
                r = hi / lo
                print(f"  {hi * 1000:9.4f} ms / {lo * 1000:9.4f} ms = "
                      f"{r:.5f}   (off an integer by "
                      f"{abs(r - round(r)) / max(round(r), 1) * 1e6:.0f} ppm)")
                print(f"    {pb if hi == gb else pa}")
                print(f"    {pa if hi == gb else pb}")
    print("=" * 72)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
