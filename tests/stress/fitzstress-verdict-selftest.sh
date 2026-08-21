#!/usr/bin/env bash
#
# Prove tests/stress/fitzstress-verdict.sh can fail.
#
#   tests/stress/fitzstress-verdict-selftest.sh
#
# The harness this scores needs a bridged guest, a peer with a Fitz server and
# hours, so the corrupt run it exists to catch cannot be produced on demand.
# It is produced here instead, as the artefacts a corrupt run leaves, and each
# one has to come out red.
#
# The fixtures are fitzstress.c's own stress-summary.txt and compare.log
# (fs_summary(), fs_compare()).  Needs nothing; under a second.
#
# SPDX-License-Identifier: MIT

set -uo pipefail
ROOT="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
# shellcheck source=tests/stress/fitzstress-verdict.sh
. "$ROOT/tests/stress/fitzstress-verdict.sh"

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

pass=0; fail=0
expect() { # name want-rc summary comparelog run-rc
    local name="$1" want="$2" got out
    out=$(fitzstress_verdict "$3" "$4" "${5:-0}"); got=$?
    if [ "$got" = "$want" ]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        echo "SELFTEST FAIL: $name wanted rc $want, got $got" >&2
        printf '%s\n' "$out" | sed 's/^/    /' >&2
    fi
}

summary() { # file stuck bad dirty
    cat > "$1" <<EOF
seconds 14400
stuck_workers $2
bad_total $3
dirty_total $4
w1 iters 812 kb 419840 errs 3 bad 0 alive 1 phase copy stamp 14399
w2 iters 774 kb 396288 errs 1 bad 0 alive 1 phase read stamp 14398
w3 iters 690 kb 353280 errs 0 bad 0 alive 1 phase churn stamp 14399
w4 iters 12 kb 6144 errs 0 bad 0 alive 1 phase tree stamp 14397
EOF
}

comparelog() { # file clean dirty
    local i
    : > "$1"
    for ((i = 0; i < $2; i++)); do
        printf '\n===== t=%d testsys%d =====\n----- rc 0 -----\n' \
               $((i * 900)) "$i" >> "$1"
    done
    for ((i = 0; i < $3; i++)); do
        printf '\n===== t=%d testsys%d =====\nfoo/bar differs\n----- rc 5 -----\n' \
               $((9000 + i * 900)) $(($2 + i)) >> "$1"
    done
}

# ---- four hours in which nothing went wrong -------------------------------
summary "$T/good.txt" 0 0 0
comparelog "$T/good.log" 16 0
expect "a clean run" 0 "$T/good.txt" "$T/good.log"

# ---- THE RUN THAT USED TO EXIT 0 ------------------------------------------
# Three trees on the share did not compare.  The old harness printed
# "13 clean, 3 not" and forwarded RETURN_OK.
summary "$T/dirty.txt" 0 0 3
comparelog "$T/dirty.log" 13 3
expect "three trees that did not compare" 1 "$T/dirty.txt" "$T/dirty.log"

# ---- bytes that came back different ---------------------------------------
summary "$T/bad.txt" 0 7 0
comparelog "$T/bad.log" 16 0
expect "seven buffers that came back wrong" 1 "$T/bad.txt" "$T/bad.log"

# ---- the freeze -----------------------------------------------------------
summary "$T/stuck.txt" 2 0 0
comparelog "$T/stuck.log" 16 0
expect "two workers that never came back" 1 "$T/stuck.txt" "$T/stuck.log"

# ---- the question that was never asked ------------------------------------
summary "$T/nocmp.txt" 0 0 0
: > "$T/nocmp.log"
expect "comparetree never ran" 1 "$T/nocmp.txt" "$T/nocmp.log"

# ---- the guest and its own log disagreeing --------------------------------
# The counter says clean and the log holds three failures, which is worse than
# either one alone: one of the two is lying about the same run.
summary "$T/liar.txt" 0 0 0
comparelog "$T/liar.log" 13 3
expect "the counter and the log disagree" 1 "$T/liar.txt" "$T/liar.log"

# ---- an artefact from a binary that could not count -----------------------
cat > "$T/old.txt" <<'EOF'
seconds 14400
stuck_workers 0
w1 iters 812 kb 419840 errs 3 bad 0 alive 1 phase copy stamp 14399
EOF
comparelog "$T/old.log" 16 0
expect "a summary from before the counters" 1 "$T/old.txt" "$T/old.log"

# ---- a run in which no worker did anything --------------------------------
cat > "$T/idle.txt" <<'EOF'
seconds 14400
stuck_workers 0
bad_total 0
dirty_total 0
w1 iters 0 kb 0 errs 0 bad 0 alive 1 phase copy stamp 0
EOF
comparelog "$T/idle.log" 1 0
expect "four workers that never completed an iteration" 1 "$T/idle.txt" "$T/idle.log"

# ---- artefacts that are not a measurement ---------------------------------
summary "$T/to.txt" 0 0 0
comparelog "$T/to.log" 16 0
expect "the host deadline expired" 3 "$T/to.txt" "$T/to.log" 124
expect "no summary at all" 3 "$T/missing.txt" "$T/good.log"

echo "fitzstress-verdict selftest: $pass passed, $fail failed"
[ "$fail" = 0 ] || exit 1
exit 0
