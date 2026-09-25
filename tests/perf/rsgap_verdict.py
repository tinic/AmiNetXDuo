#!/usr/bin/env python3
"""Verdict for docs/plans/roadshow-rx-gap.md, from run-rsgap.sh's output.

  rsgap_verdict.py [--margin 1.5] [--bootstrap 10000] [--seed 1]
                   [--min-pairs 20] [--null ARM] [FILE...]

Reads rsgap_collect.py's `rsgap_boot` and `rsgap_run` lines (stdin when no
FILE).  The boot is the unit; its value is mean_kbps, the mean of its
transfers.

Pairs are the collector's: pair=K by schedule position, boots (1,2), (3,4),
... under R/A/A/R alternation.  Nothing here re-pairs.  If any paired boot is
invalid, a scheduled boot is missing, a pair is incomplete or the run record
says expected_ok=0, the verdict is inconclusive with
reason=invalid_or_missing_boot and no interval is computed.

Each R-A difference is a percentage of AmiNetXDuo's median per-boot mean,
positive when Roadshow is faster.  The interval is a percentile bootstrap of
the mean difference over pairs.

Verdict, exactly as the protocol defines it, margin declared before any run:
  gap              the interval excludes zero and lies wholly beyond +-margin
  no_material_gap  the interval lies wholly within +-margin
  inconclusive     anything else

Below --min-pairs (20, the protocol's sample) the verdict is always
`inconclusive reason=too_few_pairs`.  What the data would be classified as is
written first on its own `dry_run_classification=` line: a procedure check
that the pipeline runs, never a result.

--null ARM reads a null control instead: each pair is two boots of that one
arm, first minus second, and the interval is reported with
verdict=null_control.  The invalid-or-missing rule applies; --min-pairs does
not.

Output is key=value lines.  Exit 0 when a verdict line was written, 2 when
the input held nothing usable.

SPDX-License-Identifier: MIT
"""

import argparse
import random
import statistics
import sys


MIN_PAIRS = 20


def kv_of(line):
    return dict(t.split("=", 1) for t in line.split()[1:] if "=" in t)


def parse(lines):
    """Measurement boots (valid or not) in boot order, and the run record."""
    boots, runrec = [], None
    for line in lines:
        line = line.strip()
        if line.startswith("rsgap_run "):
            runrec = kv_of(line)
            continue
        if not line.startswith("rsgap_boot "):
            continue
        kv = kv_of(line)
        if kv.get("mode") != "measure":
            continue
        try:
            kv["boot"] = int(kv["boot"])
            kv["mean_kbps"] = float(kv["mean_kbps"])
        except (KeyError, ValueError):
            continue
        boots.append(kv)
    boots.sort(key=lambda b: b["boot"])
    return boots, runrec


def pair_up(boots, runrec):
    """{pair id: [boot, boot]} and the defects that forbid a verdict."""
    pairs = {}
    for b in boots:
        if b.get("pair", "-") != "-":
            pairs.setdefault(b["pair"], []).append(b)
    bad = sorted(b["boot"] for p in pairs.values() for b in p
                 if b.get("valid") != "1")
    want = int(runrec.get("pairs_scheduled", "0"))
    incomplete = sorted(k for k in set(pairs) | {str(i) for i in
                                                 range(1, want + 1)}
                        if len(pairs.get(k, [])) != 2)
    return pairs, {"boots": bad, "incomplete_pairs": incomplete,
                   "missing_pos": runrec.get("missing_pos", "-"),
                   "expected_ok": runrec.get("expected_ok", "0")}


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


def run(lines, margin=1.5, nboot=10000, seed=1, null_arm=None,
        min_pairs=MIN_PAIRS, out=sys.stdout):
    boots, runrec = parse(lines)
    by_arm = {}
    for b in boots:
        if b.get("valid") == "1":
            by_arm.setdefault(b["arm"], []).append(b["mean_kbps"])
    if not by_arm:
        emit(out, verdict="none", reason="no_valid_boots")
        return 2
    for arm in sorted(by_arm):
        emit(out, arm=arm, **arm_stats(by_arm[arm]))
    if runrec is None:
        emit(out, verdict="none", reason="no_run_record")
        return 2

    pairs, defect = pair_up(boots, runrec)
    if defect["boots"] or defect["incomplete_pairs"] or \
            defect["missing_pos"] != "-" or defect["expected_ok"] != "1":
        emit(out, verdict="inconclusive", direction="none",
             reason="invalid_or_missing_boot",
             boots=",".join(map(str, defect["boots"])) or "-",
             incomplete_pairs=",".join(defect["incomplete_pairs"]) or "-",
             missing_pos=defect["missing_pos"],
             expected_ok=defect["expected_ok"])
        return 0

    ordered = [pairs[k] for k in sorted(pairs, key=int)]
    if null_arm is not None:
        ref_arm = null_arm
        ok = all(x["arm"] == y["arm"] == null_arm for x, y in ordered)
        vals = [(x["mean_kbps"], y["mean_kbps"]) for x, y in ordered]
    else:
        ref_arm = "A"
        ok = all({x["arm"], y["arm"]} == {"R", "A"} for x, y in ordered)
        vals = [(x["mean_kbps"], y["mean_kbps"]) if x["arm"] == "R" else
                (y["mean_kbps"], x["mean_kbps"]) for x, y in ordered]
    if not ordered or not ok:
        emit(out, verdict="none" if null_arm is None else "null_control",
             reason="no_pairs" if not ordered else "pair_arm_mismatch",
             pairs=len(ordered))
        return 2
    ref = [v for p in ordered for v in (p[0], p[1])
           if v["arm"] == ref_arm]
    ref_median = statistics.median(v["mean_kbps"] for v in ref)
    diffs = [100.0 * (x - y) / ref_median for x, y in vals]
    lo, hi = bootstrap_ci(diffs, nboot, seed)
    emit(out, pairs=len(vals), diff_mean_pct="%.3f" % statistics.fmean(diffs),
         diffs_pct=",".join("%.3f" % d for d in diffs),
         pair_boots=",".join("%d-%d" % (x["boot"], y["boot"])
                             for x, y in ordered),
         ci_lo_pct="%.3f" % lo, ci_hi_pct="%.3f" % hi,
         margin_pct="%.2f" % margin, ref_arm=ref_arm,
         ref_median_kbps="%.2f" % ref_median, bootstrap=nboot, seed=seed)
    if null_arm is not None:
        emit(out, verdict="null_control", arm=null_arm, pairs=len(vals),
             ci_lo_pct="%.3f" % lo, ci_hi_pct="%.3f" % hi)
        return 0
    verdict, direction = classify(lo, hi, margin)
    if len(vals) < min_pairs:
        emit(out, dry_run_classification=verdict, direction=direction,
             pairs=len(vals), min_pairs=min_pairs,
             note="procedure_check_not_a_result")
        emit(out, verdict="inconclusive", reason="too_few_pairs",
             pairs=len(vals), min_pairs=min_pairs)
        return 0
    emit(out, verdict=verdict, direction=direction, pairs=len(vals),
         ci_lo_pct="%.3f" % lo, ci_hi_pct="%.3f" % hi,
         margin_pct="%.2f" % margin)
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("files", nargs="*")
    ap.add_argument("--margin", type=float, default=1.5)
    ap.add_argument("--bootstrap", type=int, default=10000)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--min-pairs", type=int, default=MIN_PAIRS)
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
