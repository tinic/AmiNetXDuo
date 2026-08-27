#!/usr/bin/env bash
# THE REGRESSION TEST FOR SHORT NAMES AND THE SEARCH LIST.
#
#   tests/tools/run-dnssearch.sh [-m MODEL] [-t SECONDS] [-b BUILDDIR]
#                                [-B INTERFACE] [-s RESOLVER]
#
# BRIDGED.  All three phases wrote `nameserver 10.0.2.3` into
# DEVS:Internet/name_resolution, which is fs-uae's SLIRP forwarder: the search
# list was walked by the emulator's resolver and not over a wire.  The server
# is now the segment's own, read off this host's /etc/resolv.conf, and its
# answers are checked here before the guest boots -- the phases turn on
# www.example.com and example.com resolving while `.test`, the bare name and
# the stem `www.example` are NXDOMAIN, and a resolver that synthesises an
# address for a name that does not exist makes every assertion below vacuous.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

MODEL=A1200
TIMEOUT=240
BUILD="${AMINETXDUO_BUILD:-build/cm}"
IFACE="${AMINETXDUO_DNSSEARCH_IFACE:-${AMINETXDUO_AMIBERRY_BACKEND:-ens18}}"
RESOLVER="${AMINETXDUO_DNSSEARCH_RESOLVER:-}"

while getopts "m:t:b:B:s:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        B) IFACE="$OPTARG" ;;
        s) RESOLVER="$OPTARG" ;;
        *) echo "usage: $0 [-m model] [-t seconds] [-b builddir] [-B interface] [-s resolver]" >&2; exit 2 ;;
    esac
done

kv() { printf '%s=%s\n' "$1" "$2"; }
refuse() { kv reason "$1"; kv RESULT refused; exit 2; }

case "$IFACE" in
    slirp|slirp_inbound)
        refuse "slirp_walks_the_emulators_search_list: -B <interface>" ;;
esac

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

# THE SEGMENT'S OWN RESOLVER.  A bridged guest cannot reach the emulator
# host's loopback, so a systemd-resolved stub at 127.0.0.53 is refused rather
# than written into the guest's configuration and timed out.
if [ -z "$RESOLVER" ]; then
    RESOLVER=$(awk '/^nameserver/ && $2 !~ /:/ { print $2; exit }' /etc/resolv.conf)
fi
[ -n "$RESOLVER" ] ||
    refuse "this host has no IPv4 name server of its own; -s names one"
case "$RESOLVER" in
    127.*|0.0.0.0)
        refuse "$RESOLVER is on this host's loopback and a bridged guest cannot reach it; -s names one on the segment" ;;
esac

LONG="${AMINETXDUO_DNS_LONG:-www.example.com}"
SHORT="${LONG%%.*}"                       # www
DOMAIN="${LONG#*.}"                       # example.com
TLD="${DOMAIN##*.}"                       # com
STEM="${LONG%.*}"                         # www.example, which does not exist
PREFIX="${DOMAIN:0:3}"                    # exa, a prefix of a name that does
DEAD="${AMINETXDUO_DNS_DEAD:-invalid-aminetxduo.test}"
GONE="${AMINETXDUO_DNS_GONE:-nosuchhost-aminetxduo}"

GONE_MS="${AMINETXDUO_DNS_GONE_MS:-8000}"

# THE PREMISE, CHECKED BEFORE THE EMULATOR STARTS.  Every phase below is a
# statement about which of these names resolve; against a resolver that
# NXDOMAIN-hijacks, or one that cannot see the public DNS, the assertions
# still run and still say something, and what they say is untrue.
if command -v dig >/dev/null 2>&1; then
    dns_status() {
        dig +time=3 +tries=2 "@$RESOLVER" "$1" 2>/dev/null |
            sed -n 's/^;; ->>HEADER<<-.*status: \([A-Z]*\),.*/\1/p' | head -1
    }
    for want in "$LONG:NOERROR" "$DOMAIN:NOERROR" "$STEM:NXDOMAIN" \
                "$DEAD:NXDOMAIN" "$GONE:NXDOMAIN"; do
        n="${want%:*}"; expect="${want#*:}"
        got=$(dns_status "$n")
        kv "dns_$n" "${got:-nothing}"
        [ "$got" = "$expect" ] ||
            refuse "$RESOLVER answers $n with ${got:-nothing}, and this test needs $expect"
    done
else
    kv dns_premise unchecked
fi

kv resolver "$RESOLVER"
kv iface    "$IFACE"

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
    echo "==> phase $tag: booting $MODEL, a2065 bridged on $IFACE"
    sed 's/^/       /' "$stage/devs/Internet/name_resolution"

    set +e
    "$ROOT/tools/amiberry-run.sh" -N a2065 -B "$IFACE" -m "$MODEL" -t "$TIMEOUT" \
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
"nameserver $RESOLVER
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
"nameserver $RESOLVER
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
"nameserver $RESOLVER
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
    kv RESULT fail
    exit 1
fi

kv RESULT pass
exit 0
