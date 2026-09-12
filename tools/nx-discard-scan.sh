#!/usr/bin/env bash
#
# Every NetX Duo / ThreadX call in src/ and port/ whose result goes nowhere.
#
# TWO FORMS, and the second is why this file exists.  tools/check-nx-status.sh
# shipped seeing only the first:
#
#   (VOID)nx_foo(...);      a cast -- at least it is a marker
#   nx_foo(...);            a bare statement -- no marker at all
#
# The bare form is the commoner one: 141 casts against 265 statements when this
# was written, so a gate that saw casts alone reported "0 unreviewed discards"
# with more than half the surface invisible to it.  A count nobody can see
# behind is worse than no count.
#
# Prints `count<TAB>file<TAB>symbol', one row per (file, symbol) pair, sorted.
# Both the gate and its baseline read this, so they cannot disagree about what
# a discard is.
#
# NOT COUNTED, and each for a reason:
#   src/tls/           vendored Eclipse ThreadX sources, not ours to annotate
#   */test/, tests/    a test that ignores a status is testing something else
#   VOID callees       there is no result to discard; the list is derived from
#                      the vendored headers, not written here
#
# SPDX-License-Identifier: MIT

set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 1

void_list="$(mktemp)"; trap 'rm -f "$void_list"' EXIT

grep -rhoE "^VOID[[:space:]]+_?(nx|tx)_[a-z0-9_]+" \
     third_party/threadx/common/inc/*.h \
     third_party/netxduo/common/inc/*.h \
     port/threadx-amiga/inc/*.h 2>/dev/null |
    awk '{print $2}' | sort -u > "$void_list"

{
    # Form 1: the cast.
    grep -rnE "\(VOID\)[[:space:]]*_?(nx|tx)_[a-z0-9_]+[[:space:]]*\(" \
         src port --include='*.c' --include='*.h' 2>/dev/null |
        sed -E 's/^([^:]+):[0-9]+:.*\(VOID\)[[:space:]]*(_?(nx|tx)_[a-z0-9_]+).*/\1 \2/'

    # Form 2: the bare statement.  A line whose first token is the call.
    grep -rnE "^[[:space:]]*_?(nx|tx)_[a-z0-9_]+[[:space:]]*\(" \
         src port --include='*.c' 2>/dev/null |
        grep -vE ":[0-9]+:[[:space:]]*\*" |
        sed -E 's/^([^:]+):[0-9]+:[[:space:]]*(_?(nx|tx)_[a-z0-9_]+).*/\1 \2/'
} |
    grep -vE "^src/tls/|/test/|^tests/" |
    grep -vE "AMI_NX_" |
    awk -v v="$void_list" '
        BEGIN { while ((getline l < v) > 0) isvoid[l] = 1 }
        NF == 2 && !($2 in isvoid) { n[$1 "\t" $2]++ }
        END { for (k in n) printf "%d\t%s\n", n[k], k }
    ' | LC_ALL=C sort
