#!/usr/bin/env bash
# Prove ClassicWB's default mDNS identity follows its per-checkout MAC.
# SPDX-License-Identifier: MIT

set -uo pipefail
ROOT="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"

probe() { # mac [classicwb arguments]
    local mac="$1"
    shift
    AMINETXDUO_CWB_MAC="$mac" HOME=/nonexistent \
        "$ROOT/tools/classicwb.sh" "$@" 2>/dev/null || true
}

hostname_of() {
    sed -n 's/^hostname=//p' | head -1
}

one=$(probe 02:41:4d:49:12:34 | hostname_of)
two=$(probe 02:41:4d:49:56:78 | hostname_of)
named=$(probe 02:41:4d:49:12:34 -n workshop | hostname_of)

bad=0
[ "$one" = amiga-a1200-plain-491234 ] || {
    echo "wrong first default: '$one'" >&2; bad=$((bad + 1)); }
[ "$two" = amiga-a1200-plain-495678 ] || {
    echo "wrong second default: '$two'" >&2; bad=$((bad + 1)); }
[ "$one" != "$two" ] || {
    echo "two MAC addresses produced the same hostname" >&2; bad=$((bad + 1)); }
[ "$named" = workshop ] || {
    echo "-n was not preserved: '$named'" >&2; bad=$((bad + 1)); }

echo "classicwb identity selftest: $((4 - bad)) passed, $bad failed"
[ "$bad" = 0 ]
