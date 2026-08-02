#!/usr/bin/env bash
#
# The performance regression gate.  Run it before committing.
#
#   tests/perf/run-perfgate.sh [-b BUILDDIR] [-r REPS] [-k MHZ] [-T TAG]
#                              [-B] [-f BASELINE] [-h]
#
# WHY IT EXISTS
#
#   0.16.6 shipped a read-throughput regression that our own lab rig measured
#   as an improvement.  Two things let that through: nothing timed the CPU path
#   on the slowest machine we support, and nothing tested a link that loses
#   packets.  This is the first of those; -a loss is the second.
#
# WHAT IT MEASURES, AND ON WHAT
#
#   tools/winuae-run.sh -x -m A500 -k 14: WinUAE's cycle-exact 68000 with the
#   CPU clock doubled and the chipset left alone, which is the arrangement an
#   accelerated A500 has.  That model is the only CPU timing model in either
#   emulator that reproduces published cycle counts -- run-cpucal.sh is the
#   check, and this script runs it first, every time, as a guard: if the
#   profile has stopped charging 8 cycles for ADD.L then a throughput change
#   is the emulator's, not the code's, and the run aborts rather than
#   reporting a regression.
#
#   Then perf_test, whose per-primitive rows price the per-byte path and whose
#   end-to-end cases price the per-packet one.
#
# WHAT IT IS NOT
#
#   There is no network here and no packet loss, so this axis cannot see a
#   change in retransmission or acknowledgement behaviour -- which is what
#   0.16.6 actually got wrong.  It sees work added to the copy, checksum and
#   packet-plumbing paths.  The loss axis is a separate arm and needs a peer.
#
#   It is also not the user's machine.  14 MHz is an accelerator
#   configuration, the Fast RAM path is not that card's, and the SANA-II
#   driver is not in the picture at all.  Treat the numbers as an index that
#   moves with the code, not as a prediction of a rate.
#
# THRESHOLDS
#
#   tests/perf/perfgate-baseline.txt holds one line per metric:
#
#       NAME  DIRECTION  VALUE  TOLERANCE_PERCENT
#
#   DIRECTION is `lower' when smaller is better (nanoseconds per byte) and
#   `higher' when larger is (KB/s).  A metric fails when it moves the wrong way
#   by more than its tolerance; movement the right way is reported and never
#   fails.  -B rewrites the file from the current run.
#
# COST
#
#   One rep is about 100 seconds of wall clock: 18 s for the calibration guard
#   and 85 s for perf_test, both at emulated speed with warp off, because a
#   cycle count is only worth having when nothing is skipping cycles.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

BUILD="${AMINETXDUO_BUILD:-build/cm}"
REPS=1
CLOCK=14
TAG="${AMINETXDUO_RUN_TAG:-perfgate}"
RECORD=0
BASELINE="$ROOT/tests/perf/perfgate-baseline.txt"

usage() {
    cat <<'EOF'
usage: tests/perf/run-perfgate.sh [-b BUILDDIR] [-r REPS] [-k MHZ] [-T TAG]
                                  [-B] [-f BASELINE]

  -b  build tree holding cpucal and perf_test, built with
      -DAMINETXDUO_CPU=68000 (default build/cm)
  -r  repetitions; the median is compared (default 1)
  -k  CPU clock in MHz on the cycle-exact 68000 profile (default 14)
  -T  run tag, so two gates do not share a staging directory
  -B  record the current run as the new baseline instead of comparing
  -f  baseline file (default tests/perf/perfgate-baseline.txt)

The emulator is serialised on one Windows host; do not run two of these.
EOF
}

while getopts "b:r:k:T:Bf:h" opt; do
    case "$opt" in
        b) BUILD="$OPTARG" ;;
        r) REPS="$OPTARG" ;;
        k) CLOCK="$OPTARG" ;;
        T) TAG="$OPTARG" ;;
        B) RECORD=1 ;;
        f) BASELINE="$OPTARG" ;;
        h) usage; exit 0 ;;
        *) usage >&2; exit 2 ;;
    esac
done

CPUCAL="$ROOT/$BUILD/tests/perf/cpucal"
PERF="$ROOT/$BUILD/tests/perf/perf_test"

for f in "$CPUCAL" "$PERF"; do
    [ -f "$f" ] || {
        echo "missing $f" >&2
        echo "configure with -DAMINETXDUO_CPU=68000 and build cpucal perf_test" >&2
        exit 2; }
done

OUT="$ROOT/build/perfgate-$TAG"
rm -rf "$OUT"; mkdir -p "$OUT"

RUNNER=(tools/winuae-run.sh -x -m A500 -k "$CLOCK")
export AMINETXDUO_WINUAE_EXE="${AMINETXDUO_WINUAE_EXE:-C:\\winuae-patched\\winuae64.exe}"

# ------------------------------------------------------- the profile guard --
#
# ADD.L at 8 cycles and MOVE.L at 4 are the MC68000UM's figures, and a model
# that charges a flat cost per instruction cannot print both.  If either has
# moved, the emulator has changed under us and nothing below is comparable to
# the baseline.
echo "==> calibration guard"
AMINETXDUO_RUN_TAG="$TAG-cal" "${RUNNER[@]}" -t 300 "$CPUCAL" \
    > "$OUT/cpucal.txt" 2>&1 || true

guard() {
    local what="$1" want="$2" got
    got=$(sed -n "s/.*$what *[0-9.]* ns *implied *\([0-9]*\.[0-9]*\) cycles.*/\1/p" \
              "$OUT/cpucal.txt" | head -1)
    [ -n "$got" ] || { echo "FAIL: cpucal printed no $what row" >&2; exit 1; }
    echo "    $what implied $got cycles (published $want)"
    awk -v g="$got" -v w="$want" 'BEGIN { exit (g >= w * 0.97 && g <= w * 1.03) ? 0 : 1 }' \
        || { echo "FAIL: the 68000 profile is no longer cycle-exact" >&2
             echo "      $what charged $got against a published $want" >&2
             exit 1; }
}
guard "ADD.L  Dn,Dm" 8
guard "MOVE.L Dn,Dm" 4

# ------------------------------------------------------------- the metrics --
#
# Each row is NAME, then the awk that lifts it out of perf_test's output.  The
# ns/B rows are the per-byte path; the KB/s rows are per-packet plus protocol.
# perf_test's own PASS/FAIL is checked separately -- a gate that only compared
# numbers would pass a run whose transfers failed.
extract() {
    local log="$1"
    awk '
        # The value is whatever sits immediately before its unit.  Positional
        # indexing broke on the two rows whose label has a different word
        # count, so the unit is searched for instead.
        function before(unit,   i) {
            for (i = 1; i <= NF; i++) if ($i == unit) return $(i - 1)
            return ""
        }
        function emit(name, unit,   v) { v = before(unit); if (v != "") print name, v }

        /^  checksum, net68k /              { emit("checksum_net68k_nspb", "ns/B") }
        /^  n68k_copy_bytes d0 s0 /         { emit("copy_n68k_nspb",       "ns/B") }
        /^  ami_sana2_copy_bytes d0 s0 /    { emit("copy_sana2_nspb",      "ns/B") }
        /^  allocate \+ append 1460 /       { emit("append_1460_nspb",     "ns/B") }
        /^  extract_offset 1460 /           { emit("extract_1460_nspb",    "ns/B") }
        /^  loopback pipeline, net68k ck/   { emit("pipeline_net68k_nspb", "ns/B") }
        /^  nx_packet_allocate \+ release/  { emit("pkt_alloc_release_us", "us")   }

        # The end-to-end cases print their rate on the line after the label.
        /^  loopback, \+extract, net68k/    { want = "loopback_extract_kbs"; next }
        /^  wire, \+extract, net68k/        { want = "wire_extract_kbs";     next }
        want != "" && /KB\/s,/              { print want, before("KB/s,"); want = "" }
    ' "$log"
}

# Direction per metric, so a move the right way is never a failure.
direction() {
    case "$1" in
        *_kbs) echo higher ;;
        *)     echo lower ;;
    esac
}

echo "==> perf_test, $REPS rep(s)"
: > "$OUT/samples.txt"
FAILED_RUNS=0
for rep in $(seq 1 "$REPS"); do
    AMINETXDUO_RUN_TAG="$TAG-p$rep" "${RUNNER[@]}" -t 1500 "$PERF" \
        > "$OUT/perf-$rep.txt" 2>&1 || true

    grep -q "checks, .* failures -- PASS" "$OUT/perf-$rep.txt" || {
        echo "!! rep $rep: perf_test reported failures"
        grep "FAIL\|failures --" "$OUT/perf-$rep.txt" | sed 's/^/     /'
        FAILED_RUNS=$((FAILED_RUNS + 1))
    }

    extract "$OUT/perf-$rep.txt" | sed "s/^/$rep /" >> "$OUT/samples.txt"
done

[ -s "$OUT/samples.txt" ] || { echo "FAIL: perf_test produced no metrics" >&2; exit 1; }

# Median across reps, and the spread, which is the only defensible source for a
# tolerance.  A tolerance picked by hand is a guess about noise; this is a
# measurement of it.
awk '{ v[$2] = v[$2] " " $3 }
     END {
        for (k in v) {
            n = split(v[k], a, " ")
            for (i = 1; i <= n; i++) for (j = i + 1; j <= n; j++)
                if (a[j] + 0 < a[i] + 0) { t = a[i]; a[i] = a[j]; a[j] = t }
            med = (n % 2) ? a[(n + 1) / 2] : (a[n / 2] + a[n / 2 + 1]) / 2
            lo = a[1] + 0; hi = a[n] + 0
            spread = (med + 0 > 0) ? (hi - lo) * 100.0 / med : 0
            printf "%s %.3f %.2f %d\n", k, med, spread, n
        }
     }' "$OUT/samples.txt" | sort > "$OUT/median.txt"

# --------------------------------------------------------------- reporting --

if [ "$RECORD" = "1" ]; then
    {
        echo "# tests/perf/run-perfgate.sh baseline."
        echo "# NAME  DIRECTION  VALUE  TOLERANCE_PERCENT"
        echo "# Recorded on the cycle-exact 68000 profile at $CLOCK MHz."
        echo "# Tolerances are the measured spread over the recording run,"
        echo "# doubled and floored at 3%, so ordinary run-to-run movement"
        echo "# cannot fail the gate and a real regression comfortably can."
        while read -r name med spread _n; do
            tol=$(awk -v s="$spread" 'BEGIN { t = s * 2; if (t < 3) t = 3; printf "%.1f", t }')
            printf '%-24s %-7s %12s %6s\n' "$name" "$(direction "$name")" "$med" "$tol"
        done < "$OUT/median.txt"
    } > "$BASELINE"
    echo "==> baseline written to $BASELINE"
    cat "$BASELINE"
    exit 0
fi

[ -f "$BASELINE" ] || {
    echo "no baseline at $BASELINE -- record one with -B" >&2; exit 2; }

echo
printf '%-24s %12s %12s %9s %8s\n' METRIC BASELINE NOW CHANGE VERDICT
RC=0
while read -r name dir base tol; do
    case "$name" in '#'*|'') continue ;; esac

    now=$(awk -v n="$name" '$1 == n { print $2 }' "$OUT/median.txt")
    [ -n "$now" ] || { printf '%-24s %12s %12s %9s %8s\n' \
                              "$name" "$base" "-" "-" "MISSING"; RC=1; continue; }

    read -r pct verdict <<EOF
$(awk -v b="$base" -v n="$now" -v d="$dir" -v t="$tol" 'BEGIN {
        if (b + 0 == 0) { print "0.0 SKIP"; exit }
        p = (n - b) * 100.0 / b
        bad = (d == "higher") ? (p < -t) : (p > t)
        printf "%+.1f %s\n", p, bad ? "FAIL" : "ok"
     }')
EOF
    [ "$verdict" != "FAIL" ] || RC=1
    printf '%-24s %12s %12s %8s%% %8s\n' "$name" "$base" "$now" "$pct" "$verdict"
done < "$BASELINE"

echo
awk '{ printf "    %-24s median %10s  spread %s%% over %s rep(s)\n", $1, $2, $3, $4 }' \
    "$OUT/median.txt"

if [ "$FAILED_RUNS" != "0" ]; then
    echo
    echo "!! perf_test itself failed in $FAILED_RUNS of $REPS rep(s) -- read $OUT/perf-*.txt"
    RC=1
fi

echo
[ "$RC" = "0" ] && echo "==> PASS" || echo "==> FAIL"
exit "$RC"
