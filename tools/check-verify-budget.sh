#!/usr/bin/env bash
#
# The receive checksum verify must not grow.
#
#   tools/check-verify-budget.sh [build-dir]
#
# WHY THIS EXISTS.  n68k_rx_verify_sum() is the largest ESTABLISHED ours-item
# on the receive path: removing it entirely measured +3.40% (p=0.0019, 12
# rounds an arm, 2026-09-07).  For ~118 instructions on its fused fast path
# that prices ONE INSTRUCTION AT ABOUT 0.029% OF RECEIVE -- so instruction
# count is the currency here, and it is the only currency, because the rig
# resolves about 0.9% at twenty boots an arm and no single edit to this
# function is ever going to clear that.
#
# THAT IS EXACTLY THE SHAPE A REGRESSION HIDES IN.  A change that adds twenty
# instructions costs ~0.6% and NO A/B ON THIS RIG WILL SEE IT.  Several of
# them together are a percent of receive that nobody ever measured going.  A
# counted ceiling is the only instrument with the resolution for it.
#
# It counts the WHOLE function, not the fast path: every branch is reachable
# and a cold path that doubles is still a defect, and picking out the fast
# path from assembly needs a judgement this cannot make.
#
# SPDX-License-Identifier: MIT

set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 1

# The ceiling, and the number behind it.  Raise it ONLY with a reason and a
# measurement; every instruction added here is ~0.029% of receive.
BUDGET="${AMINETXDUO_VERIFY_BUDGET:-230}"

BUILD="${1:-build/wp2}"
CCJSON="$BUILD/compile_commands.json"

if [ ! -r "$CCJSON" ]; then
    echo "verify_budget=skipped reason=no_compile_commands path=$CCJSON"
    exit 0
fi

OUT=$(mktemp -d); trap 'rm -rf "$OUT"' EXIT

# The real compile line for this TU, with -c turned into -S.  Reproducing the
# flags by hand is how a gate ends up measuring a different program than the
# one that ships: this file's shape depends on FEATURE_NX_IPV6,
# AMINETXDUO_RX_VERIFY and the CPU, all of which live in that command.
if ! python3 - "$CCJSON" "$OUT/v.s" <<'PY'
import json, re, subprocess, sys
ccjson, out = sys.argv[1], sys.argv[2]
try:
    cc = json.load(open(ccjson))
except Exception as e:
    print("verify_budget=skipped reason=unreadable_compile_commands"); sys.exit(3)
hits = [x for x in cc if x["file"].endswith("n68k_rx_verify.c")]
if not hits:
    print("verify_budget=skipped reason=tu_not_in_build"); sys.exit(3)
e = hits[0]
cmd = re.sub(r"-o\s+\S+", "-o " + out, e["command"]).replace(" -c ", " -S ")
# -flto MAKES -S EMIT GIMPLE, NOT ASSEMBLY, and the symbol then does not
# appear at all -- against build/cm this gate reported symbol_not_found until
# that was found.  Counting a non-LTO compile of the same translation unit is
# the right instrument anyway: this is a RELATIVE ceiling, and a number that
# moves only when the source moves is what it needs.
cmd = re.sub(r"\s-flto(=\S+)?", " ", cmd) + " -fno-lto"
r = subprocess.run(cmd, shell=True, cwd=e["directory"],
                   capture_output=True, text=True)
if r.returncode != 0:
    print("verify_budget=FAIL reason=compile_failed")
    print(r.stderr[:400]); sys.exit(1)
PY
then
    rc=$?
    [ "$rc" = 3 ] && exit 0
    exit 1
fi

if [ ! -s "$OUT/v.s" ]; then
    echo "verify_budget=skipped reason=no_assembly"
    exit 0
fi

# Instructions between the label and the next directive that ends the body.
# STOP AT THE NEXT FUNCTION LABEL, not only at a directive.  This target's
# assembler output carries NO .size or .type at all, so a directive-only rule
# runs to end of file for any function that is not followed by a .globl -- it
# gave the right answer here only because _n68k_rx_verify_stats follows with
# one.  The same rule applied to a static function in another file counted
# four functions as one (180 against a real 62).  A label at column one
# starting with an underscore ends the body on this target; local labels are
# L1, L2 and do not.
count=$(awk '/^_n68k_rx_verify_sum:/{f=1;next}
             f && /^_[A-Za-z]/{exit}
             f && /^[ \t]*\.(size|globl|type)/{exit}
             f && /^[ \t]+[a-z]/{n++}
             END{print n+0}' "$OUT/v.s")

if [ "$count" -eq 0 ]; then
    echo "verify_budget=FAIL reason=symbol_not_found sym=_n68k_rx_verify_sum"
    echo "  The function was not in the assembly, so a zero here would be a"
    echo "  gate that passes because it cannot see its subject."
    exit 1
fi

if [ "$count" -gt "$BUDGET" ]; then
    echo "verify_budget=FAIL instructions=$count budget=$BUDGET"
    echo "  n68k_rx_verify_sum grew by $((count - BUDGET)) instructions past its"
    echo "  ceiling.  At ~0.029% of receive each that is about"
    echo "  $(awk -v n="$((count - BUDGET))" 'BEGIN{printf "%.2f", n*0.029}')% of throughput, which is BELOW what"
    echo "  the rig can measure -- which is why it is counted instead."
    exit 1
fi

echo "verify_budget=PASS instructions=$count budget=$BUDGET"
exit 0
