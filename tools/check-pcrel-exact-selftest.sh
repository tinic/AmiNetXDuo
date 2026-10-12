#!/usr/bin/env bash
# tools/check-pcrel-exact.py against a crafted link: hc2.c built at -m68020
# with a cold function in its own hunk, so main's tail call to coldpath and
# coldpath's call to the static helper are cross-section bra.l relocations.
#
#   tools/check-pcrel-exact-selftest.sh
#
# hc2-neg is hc2 with the bra.l to helper moved 14 bytes short, onto pad0:
# it still lands on a function entry, which the old heuristic accepted.
# Prints check_pcrel_exact_selftest=PASS|FAIL cases=N failures=M; exit 0/1.
# SPDX-License-Identifier: MIT

set -u

here=$(cd "$(dirname "$0")" && pwd)
fx="$here/pcrel-exact-fixture"
tool="$here/check-pcrel-exact.py"
work=$(mktemp -d "${TMPDIR:-/tmp}/pcrel-exact-selftest.XXXXXX")
trap 'rm -rf "$work"' EXIT

cases=0
failures=0

# expect NAME EXIT BINARY MAP [DIR] -- and the output line it must print.
expect() {
    local name=$1 want=$2 bin=$3 map=$4 dir=$5 line=$6 out rc
    cases=$((cases + 1))
    out=$(python3 "$tool" "$bin" "$map" "$dir" 2>&1)
    rc=$?
    if [ "$rc" -ne "$want" ] || ! printf '%s\n' "$out" | grep -q -- "$line"; then
        failures=$((failures + 1))
        echo "case=$name result=FAIL exit=$rc want_exit=$want"
        printf '%s\n' "$out"
    else
        echo "case=$name result=PASS exit=$rc"
    fi
}

expect correct 0 "$fx/hc2" "$fx/hc2.map" "$fx" \
    '^pcrel_exact=PASS .* checked=2 branches=2 ok=2 mismatches=0 unscanned=0'
expect branch-short 1 "$fx/hc2-neg" "$fx/hc2.map" "$fx" \
    '^pcrel_mismatch=hc2-neg site=0x2da kind=bra.l .* delta=-14$'

# One byte of hot() that no relocation covers, changed in the image.
python3 - "$fx/hc2" "$fx/hc2.obj" "$work/hc2-byte" <<'EOF'
import sys
img = bytearray(open(sys.argv[1], "rb").read())
obj = open(sys.argv[2], "rb").read()
# hot() starts at 0x38 in hc2.obj's first code hunk: its first word.
code = obj[obj.index(bytes.fromhex("000003e9")) + 8:]
at = img.index(code[0x38:0x40])
img[at] ^= 0x01
open(sys.argv[3], "wb").write(img)
EOF
expect byte-changed 1 "$work/hc2-byte" "$fx/hc2.map" "$fx" \
    '^pcrel_bytes_differ=hc2-byte site=0x2b0 '

# The object the map names is not there: a refusal, never a pass.
expect object-missing 2 "$fx/hc2" "$fx/hc2.map" "$work" \
    '^pcrel_unscanned=hc2.obj reason=no_object'

if [ "$failures" -eq 0 ]; then
    echo "check_pcrel_exact_selftest=PASS cases=$cases failures=0"
    exit 0
fi
echo "check_pcrel_exact_selftest=FAIL cases=$cases failures=$failures"
exit 1
