#!/usr/bin/env bash
#
# Score a BebboSSH run: compare every byte, then derive the throughput.
#
#   tests/bebbossh/check.sh <testhd-dir> <xfer-dir>
#
# WHY EVERY BYTE
#
#   docs/RESEARCH.md 79.6 records a shim bug that put a CR in front of every LF
#   in a transferred file: 64 KB came back the RIGHT LENGTH and the wrong
#   bytes, and a 45-byte case never showed it because the payload had one LF.
#   A size check is not a check.
#
# WHY THE SLOPE AND NOT THE TOTAL
#
#   Each command's elapsed time is connect + key exchange + auth + transfer,
#   and on a 14 MHz 68020 the handshake is several seconds.  The difference
#   between two sizes cancels everything fixed, so (256K - 64K) / (t256 - t64)
#   is the per-byte rate.  Two slopes are computed -- 45B to 64K and 64K to
#   256K -- and they have to agree.  If they do not, the number is a window, a
#   round-trip count or a buffer, not a per-byte cost, and calling it KB/s
#   would be wrong.
#
# THE VERDICT LINE IS THE RESULT
#
#   The emulator's exit status cannot be trusted here: a2065.device 2.16 never
#   honours AbortIO() on a pending CMD_READ, so a completed run can hang at the
#   last CloseLibrary() and be reported as a timeout.
#
# SPDX-License-Identifier: MIT

set -uo pipefail

HD="${1:?usage: check.sh <testhd-dir> <xfer-dir>}"
XFER="${2:?usage: check.sh <testhd-dir> <xfer-dir>}"

REPORT="$HD/client.txt"
FAIL=0
NCHECK=0
NOK=0

# --------------------------------------------------------------- the bytes --

check_pair() {
    local what="$1" got="$2" want="$3"
    NCHECK=$((NCHECK + 1))
    if [ ! -f "$got" ]; then
        printf '  %-28s MISSING\n' "$what"
        FAIL=1
        return
    fi
    local gs ws
    gs=$(wc -c < "$got" | tr -d ' ')
    ws=$(wc -c < "$want" | tr -d ' ')
    if cmp -s "$got" "$want"; then
        printf '  %-28s ok, %s bytes\n' "$what" "$gs"
        NOK=$((NOK + 1))
    elif [ "$gs" != "$ws" ]; then
        printf '  %-28s WRONG SIZE: %s bytes, wanted %s\n' "$what" "$gs" "$ws"
        FAIL=1
    else
        printf '  %-28s RIGHT SIZE, WRONG BYTES -- %s\n' \
               "$what" "$(cmp "$got" "$want" 2>&1 | head -1)"
        FAIL=1
    fi
}

if [ ! -f "$REPORT" ]; then
    echo "no DH0:client.txt -- the guest never ran the command list."
    echo "-------------------------------------------------------------"
    echo "VERDICT: FAIL -- no run"
    exit 1
fi

# The cipher arms present are whatever the command list ran, so they are
# discovered rather than assumed: a -C list may have run only one.
ARMS=()
for suf in gcm cha; do
    if grep -q -- "-$suf-" "$REPORT" 2>/dev/null; then ARMS+=("$suf"); fi
done
[ "${#ARMS[@]}" -gt 0 ] || ARMS=("")

name_of() { case "$1" in gcm) echo "aes128-gcm" ;; cha) echo "chacha20-poly1305" ;; *) echo "default" ;; esac; }

echo "transferred bytes, compared against the source:"
for suf in "${ARMS[@]}"; do
    [ -n "$suf" ] && p="-$suf-" || p="-"
    for sz in tiny:"45 B" mid:"64 KB" big:"256 KB"; do
        k="${sz%%:*}"; label="${sz##*:}"
        check_pair "$(name_of "$suf") h->A $label" "$HD/dn$p$k.bin"   "$XFER/$k.bin"
        check_pair "$(name_of "$suf") A->h $label" "$XFER/up$p$k.bin" "$XFER/$k.bin"
    done
done

# ------------------------------------------------------------- the timings --
#
# ClientRun writes, per command:
#     --- <the command line>
#     --- rc <n>, <s>.<cc> s
# so the seconds belong to the command named on the line before.

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

# (bytes2 - bytes1) / (t2 - t1), in KB/s.  Refuses rather than divides by a
# time difference too small to carry a rate.
slope() {
    awk -v b1="$1" -v t1="$2" -v b2="$3" -v t2="$4" 'BEGIN {
        dt = t2 - t1;
        if (dt <= 0.02) { print "n/a"; exit }
        printf "%.2f", (b2 - b1) / 1024.0 / dt;
    }'
}

echo ""
printf '%-32s %7s %7s %7s %12s %12s %8s\n' \
       "" "45 B" "64 KB" "256 KB" "45B->64K" "64K->256K" "rc"

ROWS=()
for suf in "${ARMS[@]}"; do
    [ -n "$suf" ] && p="-$suf-" || p="-"
    for dir in dn up; do
        if [ "$dir" = "dn" ]; then
            label="$(name_of "$suf")  host -> Amiga"
            pt="dn${p}tiny.bin"; pm="dn${p}mid.bin"; pb="dn${p}big.bin"
        else
            label="$(name_of "$suf")  Amiga -> host"
            pt="up${p}tiny.bin"; pm="up${p}mid.bin"; pb="up${p}big.bin"
        fi
        t1=$(elapsed_for "$pt"); t2=$(elapsed_for "$pm"); t3=$(elapsed_for "$pb")
        r1=$(rc_for "$pt");      r2=$(rc_for "$pm");      r3=$(rc_for "$pb")
        if [ -z "$t1" ] || [ -z "$t2" ] || [ -z "$t3" ]; then
            printf '%-32s incomplete -- a transfer produced no timing\n' "$label"
            FAIL=1
            continue
        fi
        s1=$(slope 45 "$t1" 65536 "$t2")
        s2=$(slope 65536 "$t2" 262144 "$t3")
        printf '%-32s %6ss %6ss %6ss %9s KB/s %9s KB/s %8s\n' \
               "$label" "$t1" "$t2" "$t3" "$s1" "$s2" "$r1/$r2/$r3"
        ROWS+=("$label|$s1|$s2")
    done
done

# The two slopes agreeing is the evidence that these are per-byte costs.  Said
# out loud rather than left for the reader to divide, because the whole point
# of measuring three sizes is thrown away if nobody checks.
echo ""
for row in "${ROWS[@]}"; do
    IFS='|' read -r l a b <<< "$row"
    awk -v l="$l" -v s1="$a" -v s2="$b" 'BEGIN {
        if (s1 == "n/a" || s2 == "n/a") { printf "  %-32s slopes not comparable\n", l; exit }
        d = (s1 > s2 ? s1 - s2 : s2 - s1) / ((s1 + s2) / 2) * 100;
        printf "  %-32s slopes agree to %.1f%%%s\n", l, d,
               (d < 5 ? "" : "   -- ABOVE 5%, these are totals, not per-byte rates");
    }'
done

echo ""
echo "The 45 B column is the handshake: connect, curve25519 twice, an ed25519"
echo "verify and an ed25519 sign.  It is not part of any KB/s figure above."
echo "-------------------------------------------------------------"
if [ "$FAIL" = "0" ] && [ "$NOK" = "$NCHECK" ]; then
    echo "VERDICT: PASS -- $NOK/$NCHECK transfers byte-identical"
    exit 0
fi
echo "VERDICT: FAIL -- $NOK/$NCHECK transfers byte-identical"
exit 1
