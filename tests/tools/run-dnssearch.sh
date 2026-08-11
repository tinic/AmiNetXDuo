#!/usr/bin/env bash
#
# THE REGRESSION TEST FOR SHORT NAMES AND THE SEARCH LIST.
#
#   tests/tools/run-dnssearch.sh [-m MODEL] [-t SECONDS] [-b BUILDDIR]
#
# WHAT IT IS PROVING
#
#   Reported from an A1200 on a real LAN: `ssh turo@playhouse2` said "no
#   address associated with name" and `ssh turo@playhouse2.local.tinic.net`
#   connected.  Then, after one successful lookup of the long name, the short
#   one worked on its own.  Two separate defects, and the second one is not
#   about short names at all.
#
#   PHASE search -- THE LIST IS WALKED.  A name with no dot is looked up as
#   given and then under each search domain in turn.  The first domain here
#   does not exist, so a pass means the list was WALKED and not merely read: a
#   resolver that tries one suffix and stops fails this, and so does the one
#   that shipped, which consulted DOMAIN and never SEARCH at all.
#
#   PHASE qualified -- A NAME WITH A DOT GETS NOTHING ADDED.  The search
#   domain is `com` and the name asked for is `www.example`, which does not
#   exist.  `www.example.com` does.  So a resolver that suffixes a name that
#   already has a dot answers this one, with the address of a machine nobody
#   asked about, and the control lookup on the next line proves the trap was
#   armed.
#
#   PHASE cache -- THE CACHE KEY.  With no search domain configured at all, a
#   short name must not resolve.  It did: NetX Duo's cache compared a query
#   against a cached name only as far as the query's own end, so once anything
#   had resolved www.example.com the cache answered `www`, and `exa` as well.
#   This phase asks for the long name FIRST on purpose -- that is the reported
#   order, and it is the only order in which the defect appears.
#
# WHAT ELSE IT ASSERTS
#
#   * a short name that does not exist fails FAST.  A search list is the kind
#     of feature that turns a mistyped name into a multi-second wait, and the
#     per-command millisecond figure ToolsSmoke prints is the only thing that
#     would notice.  A timeout here is a defect, not a slow pass.
#   * ShowNetStatus reports the list it is going to use.
#
#   The precedence between a DHCP-supplied domain and a configured one is NOT
#   here: SLIRP's DHCP server sends neither option 15 nor option 119, so there
#   is nothing on this rig to disagree with.  It is asserted deterministically
#   instead, on the exact configuration that was reported, by
#   test_search_domains() in src/config/test/test_config.c.
#
# THE ONE EXTERNAL DEPENDENCY, stated rather than hidden: SLIRP's DNS server
# at 10.0.2.3 forwards to the host's resolver, so this test needs the host to
# resolve www.example.com and example.com, and needs `.test` and the name
# `www.example` to be NXDOMAIN.  If the long name does not resolve the test
# says so and fails, rather than passing on an empty wire.
#
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

# How long `host <a short name that does not exist>` may take, in
# milliseconds, with the bare name and two suffixes to get through.  Generous
# by an order of magnitude against the round trips it should cost, and an
# order of magnitude under the 30 s a silent server used to spend.
GONE_MS="${AMINETXDUO_DNS_GONE_MS:-8000}"

FAILED=0
fail() { echo "FAIL: $*" >&2; FAILED=1; }
pass() { echo "  ok: $*"; }

# ------------------------------------------------------------- one phase ---

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

# What `host <name>` printed, or the empty string.
answer_of() { grep "^$1 has address " "$REPORT" | head -1 | awk '{print $4}' || true; }

# The `----- rc N, M ms, ... -----` line ToolsSmoke writes after a command.
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

# ============================================================ phase search ===

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

    # Resolved at all is the assertion, not resolved to a particular address:
    # $LONG is a CDN name with several A records and the two lookups need not
    # be handed the same one.  That '$SHORT' has no address of its own is what
    # the cache phase below establishes, on the same machine with the search
    # line taken away.
    if [ -n "$SHORT_ADDR" ]; then
        pass "the short name '$SHORT' resolved to $SHORT_ADDR, through the SECOND search domain"
    else
        fail "the short name '$SHORT' did not resolve: the list was not walked past $DEAD"
    fi

    # A running machine used to report its name servers and stop, so the one
    # question this report exists to answer could not be asked of it.
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

# ========================================================= phase qualified ===
#
# The suffix is `com` and the name asked for is `www.example`.  It has a dot,
# so it must go out exactly as it is and fail; `www.example.com` exists, so a
# resolver that suffixed it would answer with an address instead.

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

# ============================================================= phase cache ===
#
# No search domain at all.  The long name is asked for FIRST, which is the
# reported order and the only one the cache defect shows up in.

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
