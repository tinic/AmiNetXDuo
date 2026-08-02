#!/usr/bin/env bash
#
# Is this emulator profile cycle-accurate?  Ask it.
#
#   tests/perf/run-cpucal.sh [-b BUILDDIR] [-m MODEL] [-c CPU] [-k MHZ]
#                            [-e winuae|fsuae] [-T TAG] [-h]
#
# cpucal runs instruction sequences whose cost on real silicon is published and
# reports what the model charges.  This wraps it so the answer is a table
# rather than a run to read: the charged figure against the manufacturer's,
# and a verdict per instruction.
#
# READ tests/perf/cpucal.c BEFORE QUOTING ANY OF IT.  The primary results are
# ratios between kernels in one run, which need no clock; the implied clock at
# the end assumes the model charges ADD.L its published cost, and is decoration
# if the ratios are wrong.
#
# WHAT TO EXPECT, measured rather than assumed:
#
#   -m A500 (68000), EITHER EMULATOR   exact, and they agree with each other
#       to a tenth of a percent.  ADD.L 8.00 against a published 8, MOVE.L
#       4.00 against 4, ADDX.L 8.00 against 8.  -k 14 doubles the CPU and
#       leaves the chipset alone, and Chip RAM duly goes to 1.6x the cost of
#       Fast RAM.  This is the profile to take timings on.
#
#   FS-UAE, -m A1200 (68020)  exact on two-cycle integer work and 26% light on
#       the multiply: MULU.L charged 32 against a published 43.
#
#   WinUAE, -m A1200 (68020)  exact on two-cycle integer work and not charging
#       for the multiply at all: MULU.L 3.90 against 43.  The emulator's own
#       log calls this mode "~cycle-exact".  Do not take a multiply-bound
#       timing from it.
#
#   Anything above a 68020 under FS-UAE charges nothing for anything: cycle
#       accounting is off there and no option turns it back on.
#
# The 68000 build of cpucal has no MULU.L rows -- the part has no such
# instruction.  Its discriminator is MOVE.L at half of ADD.L, which a model
# with a flat per-instruction cost cannot produce.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

BUILD="${AMINETXDUO_BUILD:-build/cm}"
MODEL=A500
CPU=""
CLOCK=""
EMU=winuae
TAG="${AMINETXDUO_RUN_TAG:-cpucal}"

usage() {
    cat <<'EOF'
usage: tests/perf/run-cpucal.sh [-b BUILDDIR] [-m MODEL] [-c CPU] [-k MHZ]
                                [-e winuae|fsuae] [-T TAG]

  -b  build tree holding tests/perf/cpucal (default build/cm).  It must have
      been configured for the CPU the profile has: -DAMINETXDUO_CPU=68000 for
      -m A500 or -m A600.
  -m  machine profile (default A500, the one with exact 68000 timings)
  -c  override the CPU
  -k  CPU clock in MHz; needs a cycle-exact profile to mean anything
  -e  emulator.  fsuae runs locally and needs xvfb; winuae runs over ssh.
EOF
}

while getopts "b:m:c:k:e:T:h" opt; do
    case "$opt" in
        b) BUILD="$OPTARG" ;;
        m) MODEL="$OPTARG" ;;
        c) CPU="$OPTARG" ;;
        k) CLOCK="$OPTARG" ;;
        e) EMU="$OPTARG" ;;
        T) TAG="$OPTARG" ;;
        h) usage; exit 0 ;;
        *) usage >&2; exit 2 ;;
    esac
done

CPUCAL="$ROOT/$BUILD/tests/perf/cpucal"
[ -f "$CPUCAL" ] || { echo "missing $CPUCAL -- build the cpucal target" >&2; exit 2; }

OUT="$ROOT/build/cpucal-$TAG.txt"

ARGS=(-x -m "$MODEL" -t 300)
[ -z "$CPU" ]   || ARGS+=(-c "$CPU")
[ -z "$CLOCK" ] || ARGS+=(-k "$CLOCK")

case "$EMU" in
    winuae) export AMINETXDUO_WINUAE_EXE="${AMINETXDUO_WINUAE_EXE:-C:\\winuae-patched\\winuae64.exe}"
            AMINETXDUO_RUN_TAG="$TAG" tools/winuae-run.sh "${ARGS[@]}" "$CPUCAL" > "$OUT" 2>&1 || true ;;
    fsuae)  AMINETXDUO_RUN_TAG="$TAG" xvfb-run -a tools/fsuae-run.sh "${ARGS[@]}" "$CPUCAL" > "$OUT" 2>&1 || true ;;
    *)      echo "unknown emulator $EMU" >&2; exit 2 ;;
esac

grep -q "calibration probe" "$OUT" || {
    echo "cpucal produced no output; see $OUT" >&2; exit 1; }

echo "==> $EMU $MODEL${CPU:+/$CPU}${CLOCK:+ at $CLOCK MHz}"
echo

# The published column is cpucal's own -- it prints the manufacturer's figure
# beside what it measured, so the verdict here is a comparison of two numbers
# in one line rather than a table this script has to carry and keep correct.
awk '
    /implied clock/ { sub(/^ */, ""); clock = $0; next }

    /implied .* cycles/ {
        line = $0
        sub(/^  /, "", line)
        split(line, f, "implied")
        insn = f[1]
        sub(/ *[0-9.]+ ns *$/, "", insn)

        n = split(f[2], g, " ")
        charged = g[1] + 0

        published = 0
        for (i = 1; i <= n; i++) if (g[i] == "68000" || g[i] == "68020") {
            published = g[i + 1] + 0; break
        }
        if (published == 0) next

        d = (charged - published) * 100.0 / published
        v = (d < -3 || d > 3) ? ((d < -50) ? "NOT CHARGED" : "off") : "exact"
        printf "  %-22s charged %7.2f  published %3d  %+7.1f%%  %s\n",
               insn, charged, published, d, v
    }
    # ns/B, not the KB/s beside it: the Fast RAM block is printed first and the
    # Chip RAM one second, and the ratio wanted is cost, not rate.
    /read  32 KB window/ { if (!fast) fast = $(NF-3); else if (!chip) chip = $(NF-3) }

    END {
        if (clock != "") printf "\n  %s\n", clock
        if (fast && chip)
            printf "  Chip RAM / Fast RAM 32 KB read: %.2fx  "\
                   "(1.00 = the chipset is as fast as the CPU bus)\n", chip / fast
    }' "$OUT"

echo
echo "full output: $OUT"
