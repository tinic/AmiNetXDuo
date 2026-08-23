#!/usr/bin/env python3
"""Split a FitzBench profile into its read and write halves.

    tests/perf/profsplit.py fitz.prof --tools tools.txt [--ndk DIR] [--lib ...]

WHY THIS IS NOT --phase
--------------------------------------------------------------------------
tools/profiler/Profile can mark phases, but only a program that calls the
marking API produces them, and FitzBench is an ordinary benchmark that does
not.  What it DOES produce is its own per-pass timings, in EClock ticks, on
stdout, which is a better clock than the sampler's anyway.

So the windows are placed from FitzBench's own numbers, ANCHORED AT THE END.
FitzBench does one untimed write and one untimed read to warm up, then
`reps` timed (write, read) pairs, then a 64 KB verify read.  The warm-up
duration is not reported and the process prologue is not either, so anchoring
at the start would put every boundary behind an unmeasured offset.  The
epilogue, the verify read plus a Close() and two Printf()s, is the only
part of the run whose length is both short and computable, so the sequence is
laid backwards from the last sample.

The alignment is CHECKED rather than assumed: the timed passes plus the
epilogue must fit inside the sampled span with the leftover positive and no
larger than the warm-up pair it is supposed to contain.  A run that fails
that prints the discrepancy and refuses to split, because a profile split on
misplaced boundaries reads exactly as well as one split correctly.

SPDX-License-Identifier: MIT
"""

import argparse
import os
import re
import sys
from collections import defaultdict

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "..", "tools", "profiler"))
import profreport as pr                                    # noqa: E402


def fitz_timings(path, which="FITZ:"):
    """(eclock, [(kind, seconds)]) for the timed passes, in order."""
    eclock = None
    passes = []
    active = False
    with open(path, "r", errors="replace") as fh:
        for line in fh:
            m = re.search(r"fitzbench: file=(\S+)\s+bytes=(\d+).*eclock=(\d+)",
                          line)
            if m:
                active = m.group(1).startswith(which)
                if active:
                    eclock = int(m.group(3))
                    nbytes = int(m.group(2))
                    passes = []
                continue
            if not active:
                continue
            m = re.match(r"fitzbench: (write|read)\s+rep=\d+ ticks=(\d+)", line)
            if m:
                passes.append((m.group(1), int(m.group(2))))
    if eclock is None or not passes:
        raise SystemExit("no %s FitzBench timings in %s" % (which, path))
    return eclock, nbytes, [(k, t / float(eclock)) for k, t in passes]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("profile")
    ap.add_argument("--tools", required=True,
                    help="the guest's tools.txt, for FitzBench's own timings")
    ap.add_argument("--exe")
    ap.add_argument("--map", dest="mapfile")
    ap.add_argument("--objdir")
    ap.add_argument("--nm", default="m68k-amigaos-nm")
    ap.add_argument("--ndk")
    ap.add_argument("--lib", action="append", default=[])
    ap.add_argument("--idle", default=None,
                    help="explicit Exec idle address for a legacy profile; "
                         "by default idle is detected from saved SR $2000")
    ap.add_argument("--top", type=int, default=12)
    args = ap.parse_args()

    pr._NDK[0] = args.ndk
    prof = pr.Profile(args.profile)
    symtab = (pr.build_symbol_table(args.nm, args.mapfile, args.objdir)
              if args.mapfile else {})
    libspecs = [pr.parse_lib_spec(s) for s in args.lib]
    res = pr.Resolver(prof, args.exe, symtab, libspecs, args.nm)

    times = [prof.us(c) / 1e6 for c in prof.unwrap()]
    span = times[-1] if times else 0.0
    eclock, nbytes, passes = fitz_timings(args.tools)

    # The verify read is 64 KB at the last read pass's own rate.
    last_read = next(s for k, s in reversed(passes) if k == "read")
    verify = last_read * (65536.0 / nbytes)
    timed = sum(s for _k, s in passes)

    lead = span - timed - verify
    print("profile      %s" % args.profile)
    print("rate         %d Hz, %d samples over %.2f s, %d dropped"
          % (prof.rate, prof.stored, span, prof.dropped))
    gaps = getattr(prof, "flags", 0)
    if gaps & pr.OVERFLOW:
        print("!! the sample buffer filled; the tail is missing")
    print("timed passes %.2f s + %.2f s verify = %.2f s of a %.2f s span"
          % (timed, verify, timed + verify, span))
    print("lead-in      %.2f s (the prologue and the untimed warm-up pair)"
          % lead)

    warm = passes[0][1] + passes[1][1]
    if lead < 0:
        raise SystemExit("the timed passes do not FIT in the sampled span: "
                         "the boundaries cannot be placed")
    if lead > 2.5 * warm:
        raise SystemExit("lead-in %.2f s is far more than a warm-up pair "
                         "(%.2f s); refusing to split" % (lead, warm))

    # Lay the sequence out forwards from the end of the lead-in.
    bounds, t = [], lead
    for kind, secs in passes:
        bounds.append((kind, t, t + secs))
        t += secs

    # ------------------------------------------------------------ classify --

    def bucket(pc):
        name, module = res.resolve(pc)
        return name, module

    # Exec halts in `stop #$2000`.  The exception frame records the PC after
    # STOP and SR $2000, which Resolver.is_idle() uses to distinguish a halted
    # CPU from ordinary dispatcher code.  The old fallback picked the hottest
    # address in exec.library; on a busy network run that was Signal(), and it
    # silently reported real scheduler work as idle time.
    #
    # Keep --idle as an explicit address override for old profiles that do not
    # carry SR, but never guess one from the hottest Exec sample.
    idle_pc = int(args.idle, 16) if args.idle else None

    # The `fitz` handler is a different program the profiled run never loaded,
    # so its hunks are unknown and every sample in it is unattributed.  It is
    # one hot address (its memcpy), which is enough to name it.
    unhot = defaultdict(int)
    for pc, _sr, _f, _task, _t in prof.samples:
        if bucket(pc)[1] == "unattributed":
            unhot[pc] += 1
    fitz_pc = max(unhot, key=unhot.get) if unhot else None

    # A library with no seglist tag is bracketed by the hull of its jump-table
    # targets, which is a bracket around the ENTRY POINTS and not around the
    # code.  Roadshow's own body lands outside it and resolves to nothing, so
    # the named share of a foreign library is a FLOOR.  Unattributed samples
    # within NEAR of its base are almost certainly still its code; counting
    # them separately gives the ceiling without pretending to name them.
    NEAR = 512 * 1024
    libbases = [b for b, _neg, _t, nm in prof.libs if nm.startswith("bsdsocket")]

    def classify(pc, sr):
        name, module = bucket(pc)
        if ((idle_pc is not None and pc == idle_pc) or
                (idle_pc is None and res.is_idle(pc, sr))):
            return "Exec idle loop"
        if module == "exec.library":
            return "Exec, real work"
        if module.endswith(".device") and "a2065" in module:
            return "a2065.device"
        if module.startswith("bsdsocket"):
            return "bsdsocket.library (named)"
        if module in ("FitzBench", "Fitz", "fitz") or module.startswith("Fitz"):
            return "FitzBench"
        if module == "unattributed":
            if pc == fitz_pc:
                return "fitz handler"
            if any(abs(pc - b) < NEAR for b in libbases):
                return "near the stack library, unnamed"
        if module == "timer.device":
            return "timer.device"
        return "everything else"

    ORDER = ["Exec idle loop", "bsdsocket.library (named)",
             "near the stack library, unnamed", "Exec, real work",
             "a2065.device", "timer.device", "FitzBench", "fitz handler",
             "everything else"]

    per = {"read": defaultdict(int), "write": defaultdict(int)}
    fns = {"read": defaultdict(int), "write": defaultdict(int)}
    counts = {"read": 0, "write": 0}
    for (pc, sr, _f, _task, _t), ts in zip(prof.samples, times):
        for kind, lo, hi in bounds:
            if lo <= ts < hi:
                per[kind][classify(pc, sr)] += 1
                nm, mod, _idle = res.resolve_sample(pc, sr)
                fns[kind][(nm, mod)] += 1
                counts[kind] += 1
                break

    if idle_pc is not None:
        print("idle         $%08x, %s" % (idle_pc, res.resolve(idle_pc)[0]))
    else:
        print("idle         saved SR $2000 in exec.library (STOP #$2000)")
    print()
    print("%-22s %14s %14s" % ("", "read", "write"))
    print("%-22s %14s %14s"
          % ("", "%d samples" % counts["read"], "%d samples" % counts["write"]))
    print("-" * 52)
    for k in ORDER:
        r = per["read"][k]
        w = per["write"][k]
        if not r and not w:
            continue
        print("%-22s %13.1f%% %13.1f%%"
              % (k, 100.0 * r / max(counts["read"], 1),
                 100.0 * w / max(counts["write"], 1)))
    print("-" * 52)
    for kind in ("read", "write"):
        busy = counts[kind] - per[kind]["Exec idle loop"]
        print("%s: %d busy samples, %.1f%% of the arm"
              % (kind, busy, 100.0 * busy / max(counts[kind], 1)))

    for kind in ("read", "write"):
        print()
        print("== %s, top %d ==" % (kind, args.top))
        for (nm, mod), n in sorted(fns[kind].items(), key=lambda kv: -kv[1])[:args.top]:
            print("  %-44s %-22s %6d %5.1f%%"
                  % (nm[:44], mod[:22], n, 100.0 * n / max(counts[kind], 1)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
