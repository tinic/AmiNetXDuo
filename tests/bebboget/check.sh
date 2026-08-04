#!/usr/bin/env bash
#
# Score a bebboget run: compare every byte, then derive the throughput.
#
#   tests/bebboget/check.sh <testhd-dir> <www-dir>
#
# Same shape as tests/bebbossh/check.sh and for the same reasons: a size check
# is not a check (docs/RESEARCH.md 79.6), and the difference between two sizes
# is the only figure free of the TLS handshake.  Two clients are scored side by
# side, bebboget's own TLS and our tls.library, from one run.
#
# SPDX-License-Identifier: MIT

set -uo pipefail

HD="${1:?usage: check.sh <testhd-dir> <www-dir>}"
WWW="${2:?usage: check.sh <testhd-dir> <www-dir>}"

REPORT="$HD/client.txt"
FAIL=0
NCHECK=0
NOK=0

if [ ! -f "$REPORT" ]; then
    echo "no DH0:client.txt -- the guest never ran the command list."
    echo "-------------------------------------------------------------"
    echo "VERDICT: FAIL -- no run"
    exit 1
fi

check_pair() {
    local what="$1" got="$2" want="$3"
    grep -q -- "$(basename "$got")" "$REPORT" || return 0
    NCHECK=$((NCHECK + 1))
    if [ ! -f "$got" ]; then
        printf '  %-28s MISSING\n' "$what"; FAIL=1; return
    fi
    local gs ws
    gs=$(wc -c < "$got" | tr -d ' ')
    ws=$(wc -c < "$want" | tr -d ' ')
    if cmp -s "$got" "$want"; then
        printf '  %-28s ok, %s bytes\n' "$what" "$gs"
        NOK=$((NOK + 1))
    elif [ "$gs" != "$ws" ]; then
        printf '  %-28s WRONG SIZE: %s bytes, wanted %s\n' "$what" "$gs" "$ws"; FAIL=1
    else
        printf '  %-28s RIGHT SIZE, WRONG BYTES -- %s\n' "$what" "$(cmp "$got" "$want" 2>&1 | head -1)"
        FAIL=1
    fi
}

echo "downloaded bytes, compared against the source:"
for arm in bg:bebboget ft:fetch; do
    pre="${arm%%:*}"; who="${arm##*:}"
    for sz in tiny:"45 B" mid:"64 KB" big:"256 KB"; do
        k="${sz%%:*}"
        check_pair "$who ${sz##*:}" "$HD/$pre-$k.bin" "$WWW/$k.bin"
    done
done

elapsed_for() {
    awk -v pat="$1" '
        /^--- / && $0 !~ /^--- rc/ { cmd = $0; next }
        /^--- rc/ && index(cmd, pat) {
            for (i = 1; i <= NF; i++) if ($i ~ /^[0-9]+\.[0-9]+$/) { print $i; exit }
        }' "$REPORT"
}
rc_for() {
    awk -v pat="$1" '
        /^--- / && $0 !~ /^--- rc/ { cmd = $0; next }
        /^--- rc/ && index(cmd, pat) { r = $3; sub(/,$/, "", r); print r; exit }' "$REPORT"
}
slope() {
    awk -v b1="$1" -v t1="$2" -v b2="$3" -v t2="$4" 'BEGIN {
        dt = t2 - t1;
        if (dt <= 0.02) { print "n/a"; exit }
        printf "%.2f", (b2 - b1) / 1024.0 / dt;
    }'
}

echo ""
printf '%-14s %7s %7s %7s %12s %12s %8s\n' \
       "" "45 B" "64 KB" "256 KB" "45B->64K" "64K->256K" "rc"

ROWS=()
for arm in bg:bebboget ft:fetch; do
    pre="${arm%%:*}"; who="${arm##*:}"
    t1=$(elapsed_for "$pre-tiny.bin"); t2=$(elapsed_for "$pre-mid.bin"); t3=$(elapsed_for "$pre-big.bin")
    r1=$(rc_for "$pre-tiny.bin");      r2=$(rc_for "$pre-mid.bin");      r3=$(rc_for "$pre-big.bin")
    [ -z "$t1$t2$t3" ] && continue
    if [ -z "$t1" ] || [ -z "$t2" ] || [ -z "$t3" ]; then
        printf '%-14s partial -- only some sizes ran, no slope\n' "$who"
        continue
    fi
    s1=$(slope 45 "$t1" 65536 "$t2")
    s2=$(slope 65536 "$t2" 262144 "$t3")
    printf '%-14s %6ss %6ss %6ss %9s KB/s %9s KB/s %8s\n' \
           "$who" "$t1" "$t2" "$t3" "$s1" "$s2" "$r1/$r2/$r3"
    ROWS+=("$who|$s1|$s2")
done

echo ""
for row in "${ROWS[@]}"; do
    IFS='|' read -r l a b <<< "$row"
    awk -v l="$l" -v s1="$a" -v s2="$b" 'BEGIN {
        if (s1 == "n/a" || s2 == "n/a") { printf "  %-14s slopes not comparable\n", l; exit }
        d = (s1 > s2 ? s1 - s2 : s2 - s1) / ((s1 + s2) / 2) * 100;
        printf "  %-14s slopes agree to %.1f%%%s\n", l, d,
               (d < 5 ? "" : "   -- ABOVE 5%, these are totals, not per-byte rates");
    }'
done

echo ""
echo "The 45 B column is the TLS handshake, not a transfer rate.  Both arms"
echo "skip certificate verification, so neither number includes chain checking."
echo "-------------------------------------------------------------"
if [ "$FAIL" = "0" ] && [ "$NOK" = "$NCHECK" ] && [ "$NCHECK" -gt 0 ]; then
    echo "VERDICT: PASS -- $NOK/$NCHECK downloads byte-identical"
    exit 0
fi
echo "VERDICT: FAIL -- $NOK/$NCHECK downloads byte-identical"
exit 1
