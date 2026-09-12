#!/usr/bin/env bash
#
# A SLOT THAT IS REMOVED IS CLEARED IN EVERY FIELD.
#
# src/netstack keeps its per-interface state in arrays indexed by NX interface
# slot, and slots are REUSED: AddNetInterface after a RemoveNetInterface, and
# routinely ami_ns_take_interface_slot(), where an interface nobody named gives
# up its place to one somebody did.  Whatever a removal leaves behind is what
# the next interface in that slot starts life believing about itself.
#
# ami_ns_interface_remove_locked() used to clear two of the nine.  The one that
# showed was ns_IfaceMdnsSvc: ami_netstack_mdns_enable() registers services
# only `if (!ns->ns_IfaceMdnsSvc[index])', and nothing else cleared it but
# ami_netstack_mdns_stop() at stack teardown -- so an interface taking over a
# slot announced none of its own services, and the guard reported success.
# ns_DhcpState and ns_LastAddress were the same shape, quieter.
#
# THE FIELD LIST IS DERIVED, NOT WRITTEN HERE.  It is every
# `ns_X[AMI_CFG_MAX_ATTACHED]' the header declares, so a TENTH array added
# without a clear fails this rather than joining the three that were missing.
#
# SPDX-License-Identifier: MIT

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 1

HDR=src/netstack/netstack_internal.h
SRC=src/netstack/netstack.c
FN=ami_ns_interface_remove_locked

for f in "$HDR" "$SRC"; do
    if [ ! -r "$f" ]; then
        echo "slot_clear=FAIL reason=missing file=$f" >&2
        exit 1
    fi
done

# Every per-slot array the stack declares.
#
# A while-read loop and NOT mapfile: the macOS runner is bash 3.2, where
# mapfile does not exist.  It failed there with "mapfile: command not found"
# and took the build with it.  Same shape as writing the ctest transcript
# parser with GNU sed's \(a\|b\), which BSD sed does not have: nothing a
# stage macOS runs may use a bash 4 builtin or a GNU-only expression.
FIELDS=""
while IFS= read -r _f; do
    [ -n "$_f" ] && FIELDS="$FIELDS $_f"
done < <(sed -n 's/^[[:space:]]*[A-Za-z_][A-Za-z_0-9 *]*[ *]\(ns_[A-Za-z_0-9]*\)\[AMI_CFG_MAX_ATTACHED\];.*/\1/p' "$HDR" | sort -u)

# shellcheck disable=SC2086
set -- $FIELDS
nfields=$#

if [ "$nfields" -lt 5 ]; then
    echo "slot_clear=FAIL reason=found_${nfields}_fields header=$HDR" >&2
    echo "!! Fewer per-slot arrays than this tree has ever had.  The header's" >&2
    echo "!! shape changed and this gate is reading it wrong, so it is failing" >&2
    echo "!! rather than passing on a list it did not really find." >&2
    exit 1
fi

# The body of the removal, from its definition to the closing brace at column 0.
body=$(awk -v fn="$FN" '
    $0 ~ "^[A-Za-z_].*[ *]" fn "\\(" { inside = 1 }
    inside { print }
    inside && /^\}/ { exit }
' "$SRC")

if [ -z "$body" ]; then
    echo "slot_clear=FAIL reason=no_function fn=$FN src=$SRC" >&2
    echo "!! $FN() was not found.  It was renamed or moved; this gate cannot" >&2
    echo "!! see what it guards and says so rather than passing." >&2
    exit 1
fi

# ns_IfaceClaims is the one field a removal must NOT have to clear, and the
# exemption is EARNED rather than declared: the function refuses outright when
# the count is not zero, so by the time it clears anything the count is already
# zero.  If that guard ever goes, the exemption goes with it and the field is
# required like the rest.
claims_guarded=0
if printf '%s\n' "$body" |
       grep -qE "ns_IfaceClaims\\[[A-Za-z_0-9]*\\][[:space:]]*!=[[:space:]]*0"; then
    claims_guarded=1
fi

missing=""
for f in $FIELDS; do
    if [ "$f" = ns_IfaceClaims ] && [ "$claims_guarded" = 1 ]; then
        continue
    fi
    if ! printf '%s\n' "$body" | grep -qE "\\b$f\\[[A-Za-z_0-9]*\\][[:space:]]*="; then
        missing="$missing $f"
    fi
done

if [ -n "$missing" ]; then
    echo "slot_clear=FAIL fields=$nfields uncleared:$missing" >&2
    echo "!! $FN() leaves those fields as the previous interface left them," >&2
    echo "!! and the slot is reused.  Clear every per-slot field there." >&2
    exit 1
fi

echo "slot_clear=PASS fields=$nfields cleared in $FN (ns_IfaceClaims exempt: guarded=$claims_guarded)"
