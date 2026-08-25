#!/usr/bin/env bash
# THE REGRESSION TEST FOR SHORT NAMES AND THE SEARCH LIST.
# at 10.0.2.3 forwards to the host's resolver, so this test needs the host to
# resolve www.example.com and example.com, and needs `.test` and the name
# `www.example` to be NXDOMAIN.  If the long name does not resolve the test
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

MODEL=A1200
TIMEOUT=240
BUILD="${AMINETXDUO_BUILD:-build/cm}"

while getopts "m:t:b:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        *) echo "usage: $0 [-m model] [-t seconds] [-b builddir]" >&2; exit 2 ;;
    esac
done

TOOLS="$ROOT/$BUILD/src/tools"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"

for f in "$TOOLS/ToolsSmoke" "$TOOLS/AddNetInterface" "$TOOLS/host" \
         "$TOOLS/ShowNetStatus" "$BSD"; do
    [ -f "$f" ] || { echo "missing $f, build the tree first" >&2; exit 2; }
done

A2065="${AMINETXDUO_A2065:-}"
if [ -z "$A2065" ]; then
    for candidate in \
        "$ROOT/build/a2065.device" \
        "$HOME/amiga-assets/devs/a2065.device" \
        "$HOME/amiga-os-src/os-source/other_networking/sana2/bin/devs/a2065.device"
    do
        [ -f "$candidate" ] && { A2065="$candidate"; break; }
    done
fi
[ -n "$A2065" ] && [ -f "$A2065" ] || {
    echo "No a2065.device found. Set AMINETXDUO_A2065=<path>." >&2
    exit 2
}

LONG="${AMINETXDUO_DNS_LONG:-www.example.com}"
SHORT="${LONG%%.*}"                       # www
DOMAIN="${LONG#*.}"                       # example.com
TLD="${DOMAIN##*.}"                       # com
STEM="${LONG%.*}"                         # www.example, which does not exist
PREFIX="${DOMAIN:0:3}"                    # exa, a prefix of a name that does
DEAD="${AMINETXDUO_DNS_DEAD:-invalid-aminetxduo.test}"
GONE="${AMINETXDUO_DNS_GONE:-nosuchhost-aminetxduo}"

GONE_MS="${AMINETXDUO_DNS_GONE_MS:-8000}"

FAILED=0
fail() { echo "FAIL: $*" >&2; FAILED=1; }
pass() { echo "  ok: $*"; }

REPORT=""

run_phase()
{
    local tag="$1" resolver="$2"
    shift 2

    local stage="$ROOT/build/dnssearch-stage-$tag"

    rm -rf "$stage"
    mkdir -p "$stage/libs"
    cp -R "$ROOT/tests/netstack/devs" "$stage/devs"
    cp "$A2065" "$stage/devs/a2065.device"
    cp "$BSD"   "$stage/libs/bsdsocket.library"
    for t in AddNetInterface host ShowNetStatus; do
        cp "$TOOLS/$t" "$stage/$t"
    done

    printf '%s\n' "$resolver" > "$stage/devs/Internet/name_resolution"

    : > "$stage/commands.txt"
    for c in "$@"; do
        echo "$c" >> "$stage/commands.txt"
    done

    export AMINETXDUO_RUN_TAG="dnssearch-$tag"
    local hd="$ROOT/build/amiberry-testhd-$AMINETXDUO_RUN_TAG"

    echo
    echo "==> phase $tag: booting $MODEL with the A2065 on SLIRP"
    sed 's/^/       /' "$stage/devs/Internet/name_resolution"

    set +e
    "$ROOT/tools/amiberry-run.sh" -N a2065 -m "$MODEL" -t "$TIMEOUT" \
        "$TOOLS/ToolsSmoke" "$stage/commands.txt" "$stage/devs" "$stage/libs" \
        "$stage/AddNetInterface" "$stage/host" "$stage/ShowNetStatus" \
        > "$ROOT/build/dnssearch-$tag.log" 2>&1
    local rc=$?
    set -e

    REPORT="$hd/tools.txt"
    if [ ! -s "$REPORT" ]; then
        fail "phase $tag: the guest wrote no $REPORT (run rc=$rc, see build/dnssearch-$tag.log)"
        REPORT=""
        return 0
    fi

    echo
    echo "=============== phase $tag, what the commands printed ==============="
    cat "$REPORT"
    echo "====================================================================="

    local starts
    starts=$(grep -c "AddNetInterface eth0 =====" "$REPORT" || true)
    [ "$starts" -eq 1 ] || fail "phase $tag: the command list ran $starts times, the machine reset"
}

answer_of() { grep "^$1 has address " "$REPORT" | head -1 | awk '{print $4}' || true; }

elapsed_of()
{
    awk -v want="SYS:host $1 =====" '
        index($0, want) { armed = 1; next }
        armed && match($0, /rc -?[0-9]+, [0-9]+ ms/) {
            s = substr($0, RSTART, RLENGTH)
            gsub(/[^0-9]/, " ", s)
            n = split(s, f, " ")
            if (n >= 1) print f[n]
            exit
        }' "$REPORT"
}

run_phase search \
"nameserver 10.0.2.3
search $DEAD $DOMAIN
hostname amiga" \
    "SYS:AddNetInterface eth0" \
    "SYS:ShowNetStatus" \
    "SYS:host $SHORT" \
    "SYS:host $LONG" \
    "SYS:host $GONE"

if [ -n "$REPORT" ]; then
    LONG_ADDR=$(answer_of "$LONG")
    SHORT_ADDR=$(answer_of "$SHORT")

    if [ -z "$LONG_ADDR" ]; then
        fail "$LONG did not resolve at all (can this host resolve it?)"
    else
        pass "$LONG resolved to $LONG_ADDR"
    fi

    if [ -n "$SHORT_ADDR" ]; then
        pass "the short name '$SHORT' resolved to $SHORT_ADDR, through the SECOND search domain"
    else
        fail "the short name '$SHORT' did not resolve: the list was not walked past $DEAD"
    fi

    if grep -q "^Search: *$DEAD" "$REPORT" &&
       grep -q "^ *$DOMAIN\$" "$REPORT"; then
        pass "ShowNetStatus on a RUNNING machine reports both search domains"
    else
        fail "ShowNetStatus did not report the search list"
    fi

    if [ -n "$(answer_of "$GONE")" ]; then
        fail "$GONE resolved, and it does not exist"
    else
        pass "$GONE did not resolve"
    fi

    MS=$(elapsed_of "$GONE")
    if [ -n "$MS" ] && [ "$MS" -le "$GONE_MS" ]; then
        pass "$GONE failed in $MS ms, inside the ${GONE_MS} ms budget"
    else
        fail "$GONE took ${MS:-no} ms against a ${GONE_MS} ms budget: a search list must not turn a mistyped name into a wait"
    fi
fi

run_phase qualified \
"nameserver 10.0.2.3
search $TLD
hostname amiga" \
    "SYS:AddNetInterface eth0" \
    "SYS:host $STEM" \
    "SYS:host $LONG"

if [ -n "$REPORT" ]; then
    STEM_ADDR=$(answer_of "$STEM")
    LONG_ADDR=$(answer_of "$LONG")

    if [ -z "$LONG_ADDR" ]; then
        fail "$LONG did not resolve, so the '$STEM' assertion above proves nothing"
    else
        pass "the trap is armed: $STEM.$TLD is $LONG and resolves to $LONG_ADDR"
    fi

    if [ -z "$STEM_ADDR" ]; then
        pass "'$STEM' did not resolve; a name that already has a dot took no suffix"
    else
        fail "'$STEM' resolved to $STEM_ADDR; a qualified name acquired the '$TLD' suffix"
    fi
fi

run_phase cache \
"nameserver 10.0.2.3
hostname amiga" \
    "SYS:AddNetInterface eth0" \
    "SYS:host $LONG" \
    "SYS:host $SHORT" \
    "SYS:host $DOMAIN" \
    "SYS:host $PREFIX"

if [ -n "$REPORT" ]; then
    LONG_ADDR=$(answer_of "$LONG")

    if [ -z "$LONG_ADDR" ]; then
        fail "$LONG did not resolve at all (can this host resolve it?)"
    else
        pass "$LONG resolved to $LONG_ADDR"
    fi

    SHORT_ADDR=$(answer_of "$SHORT")
    if [ -z "$SHORT_ADDR" ]; then
        pass "'$SHORT' did not resolve, with no search domain to resolve it under"
    else
        fail "'$SHORT' resolved to $SHORT_ADDR with no search domain configured: the cache answered it out of the entry for $LONG"
    fi

    if [ -z "$(answer_of "$DOMAIN")" ]; then
        fail "$DOMAIN did not resolve"
    else
        pass "$DOMAIN resolved"
    fi

    PREFIX_ADDR=$(answer_of "$PREFIX")
    if [ -z "$PREFIX_ADDR" ]; then
        pass "'$PREFIX' did not resolve, and it is only a prefix of $DOMAIN"
    else
        fail "'$PREFIX' resolved to $PREFIX_ADDR: the cache matched a prefix of $DOMAIN"
    fi
fi

echo
if [ "$FAILED" -ne 0 ]; then
    echo "dnssearch: FAILED" >&2
    exit 1
fi

echo "dnssearch: PASSED"
exit 0
