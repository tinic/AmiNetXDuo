#!/usr/bin/env python3
"""Verdict for docs/plans/roadshow-rx-gap.md, from run-rsgap.sh's output.

  rsgap_verdict.py [--margin 1.5] [--bootstrap 10000] [--seed 1]
                   [--null ARM] [FILE...]

Reads `rsgap_boot ...` lines (stdin when no FILE).  A boot counts when
mode=measure and valid=1; its value is mean_kbps, the mean of its transfers.
The boot is the unit.

Paired R-A: the i-th valid R boot with the i-th valid A boot, in boot order,
which under R/A/A/R alternation pairs each boot with its neighbour.  Each
difference is expressed as a percentage of AmiNetXDuo's median per-boot mean,
positive when Roadshow is faster.  The interval is a percentile bootstrap of
the mean difference over pairs.

Verdict, exactly as the protocol defines it, margin declared before any run:
  gap              the interval excludes zero and lies wholly beyond +-margin
  no_material_gap  the interval lies wholly within +-margin
  inconclusive     anything else, and fewer than --min-pairs pairs

--null ARM reads a null control instead: consecutive boots of that one arm are
paired (1st-2nd, 3rd-4th, ...) and the same interval is reported, with
verdict=null_control.

Output is key=value lines.  Exit 0 when a verdict line was written, 2 when
the input held nothing usable.

SPDX-License-Identifier: MIT
"""

import argparse
import random
import statistics
import sys


def parse(lines):
    boots = []
    for line in lines:
        line = line.strip()
        if not line.startswith("rsgap_boot "):
            continue
        kv = {}
        for tok in line.split()[1:]:
            if "=" in tok:
                k, v = tok.split("=", 1)
                kv[k] = v
        if kv.get("mode") != "measure" or kv.get("valid") != "1":
            continue
        try:
            boots.append((int(kv["boot"]), kv["arm"], float(kv["mean_kbps"])))
        except (KeyError, ValueError):
            continue
    boots.sort()
    return boots


def arm_stats(vals):
    mean = statistics.fmean(vals)
    sd = statistics.stdev(vals) if len(vals) > 1 else 0.0
    return {
        "boots": len(vals),
        "mean_kbps": "%.2f" % mean,
        "median_kbps": "%.2f" % statistics.median(vals),
        "sd_kbps": "%.2f" % sd,
        "min_kbps": "%.2f" % min(vals),
        "max_kbps": "%.2f" % max(vals),
        "spread_pct": "%.2f" % (100.0 * (max(vals) - min(vals)) / mean),
        "cv_pct": "%.2f" % (100.0 * sd / mean),
    }


def bootstrap_ci(diffs, n, seed):
    rng = random.Random(seed)
    k = len(diffs)
    means = sorted(sum(rng.choice(diffs) for _ in range(k)) / k
                   for _ in range(n))
    lo = means[int(0.025 * (n - 1))]
    hi = means[int(0.975 * (n - 1))]
    return lo, hi


def classify(lo, hi, margin):
    """The protocol's three-way rule; returns (verdict, direction)."""
    if lo > margin:
        return "gap", "roadshow_faster"
    if hi < -margin:
        return "gap", "aminetxduo_faster"
    if -margin <= lo and hi <= margin:
        return "no_material_gap", "none"
    return "inconclusive", "none"


def emit(out, **kv):
    out.write("rsgap_verdict " +
              " ".join("%s=%s" % (k, v) for k, v in kv.items()) + "\n")


def run(lines, margin=1.5, nboot=10000, seed=1, null_arm=None, min_pairs=2,
        out=sys.stdout):
    boots = parse(lines)
    by_arm = {}
    for _, arm, v in boots:
        by_arm.setdefault(arm, []).append(v)
    if not boots:
        emit(out, verdict="none", reason="no_valid_boots")
        return 2
    for arm in sorted(by_arm):
        emit(out, arm=arm, **arm_stats(by_arm[arm]))

    if null_arm is not None:
        vals = by_arm.get(null_arm, [])
        pairs = [(vals[i], vals[i + 1]) for i in range(0, len(vals) - 1, 2)]
        ref_arm = null_arm
    else:
        r, a = by_arm.get("R", []), by_arm.get("A", [])
        pairs = list(zip(r, a))
        ref_arm = "A"
    ref = by_arm.get(ref_arm, [])
    if not ref or len(pairs) < 1:
        emit(out, verdict="none" if null_arm is None else "null_control",
             reason="no_pairs", pairs=len(pairs))
        return 2
    ref_median = statistics.median(ref)
    diffs = [100.0 * (x - y) / ref_median for x, y in pairs]
    lo, hi = bootstrap_ci(diffs, nboot, seed)
    emit(out, pairs=len(pairs), diff_mean_pct="%.3f" % statistics.fmean(diffs),
         diffs_pct=",".join("%.3f" % d for d in diffs),
         ci_lo_pct="%.3f" % lo, ci_hi_pct="%.3f" % hi,
         margin_pct="%.2f" % margin, ref_arm=ref_arm,
         ref_median_kbps="%.2f" % ref_median, bootstrap=nboot, seed=seed)
    if null_arm is not None:
        emit(out, verdict="null_control", arm=null_arm, pairs=len(pairs),
             ci_lo_pct="%.3f" % lo, ci_hi_pct="%.3f" % hi)
        return 0
    if len(pairs) < min_pairs:
        emit(out, verdict="inconclusive", direction="none",
             reason="too_few_pairs", pairs=len(pairs), min_pairs=min_pairs)
        return 0
    verdict, direction = classify(lo, hi, margin)
    emit(out, verdict=verdict, direction=direction, pairs=len(pairs),
         ci_lo_pct="%.3f" % lo, ci_hi_pct="%.3f" % hi,
         margin_pct="%.2f" % margin)
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("files", nargs="*")
    ap.add_argument("--margin", type=float, default=1.5)
    ap.add_argument("--bootstrap", type=int, default=10000)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--min-pairs", type=int, default=2)
    ap.add_argument("--null", dest="null_arm", choices=["A", "R"])
    a = ap.parse_args()
    lines = []
    if a.files:
        for f in a.files:
            with open(f) as fh:
                lines.extend(fh)
    else:
        lines = sys.stdin.readlines()
    return run(lines, a.margin, a.bootstrap, a.seed, a.null_arm, a.min_pairs)


if __name__ == "__main__":
    sys.exit(main())
