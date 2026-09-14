#!/usr/bin/env bash
#
# THIRD-PARTY RESOLVER CLIENTS: c-ares adig and ahost, as shipped on Aminet.
#
#   tests/tools/run-ahost.sh [-m MODEL] [-t SECONDS] [-b BUILDDIR]
#                            [-N BOARD] [-B BACKEND]
#
# WHY THESE TWO BINARIES.  docs/aminet-survey ranks real Aminet software by
# how much of bsdsocket.library it calls that nothing else we run calls, and
# these bring four vectors that 30 years of Amiga software uses and no test of
# ours ever touched:
#
#   ObtainSocket        173 Aminet callers, 0 of our tools
#   Dup2Socket           83 Aminet callers, 0 of our tools
#   SocketBaseTagList   429 Aminet callers, 0 of our tools
#   ProcessIsServer      12 Aminet callers, 0 of our tools
#
# That set is the socket HANDOFF family -- a program that passes an open
# socket to another process -- and the micro profile is allowed to stub it.
# Nothing proved what stubbing it costs until this arm existed.  The binaries
# also reach Errno and IoctlSocket, and `adig +tcp' asks over TCP rather than
# UDP, which is a resolver path our own commands never take.
#
# THE ONE EXTERNAL DEPENDENCY, stated rather than hidden: the name server in
# AMINETXDUO_DNS_STATIC has to answer for the name in AMINETXDUO_AHOST_NAME.
# Defaults are the LAN router and www.example.com, the same pair
# tests/tools/run-dns.sh uses.  The host resolves the name too and the guest's
# answer is checked against it, so a lookup that quietly returns the wrong
# address fails rather than passing for having printed something.
#
# BRIDGED, NEVER SLIRP: a resolver behind NAT tests the emulator's DNS.
#
# The binaries are not in this repository.  They live in the asset store,
# under apps/adig, from Aminet comm/tcp/adig.lha.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

MODEL=A1200
TIMEOUT=420
BUILD="${AMINETXDUO_BUILD:-build/cm}"
BOARD="${AMINETXDUO_AMIBERRY_BOARD:-a2065}"
IFACE="${AMINETXDUO_AMIBERRY_BACKEND:-ens18}"
STATIC_DNS="${AMINETXDUO_DNS_STATIC:-192.168.1.1}"
NAME="${AMINETXDUO_AHOST_NAME:-www.example.com}"

while getopts "m:t:b:N:B:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        N) BOARD="$OPTARG" ;;
        B) IFACE="$OPTARG" ;;
        *) echo "usage: $0 [-m model] [-t seconds] [-b builddir]\
 [-N board] [-B backend]" >&2; exit 2 ;;
    esac
done

case "$IFACE" in
    slirp|slirp_inbound|none)
        echo "ahost_backend=refused:$IFACE" >&2
        echo "This harness is bridged only: a resolver behind NAT is" >&2
        echo "answering from the emulator, not from the name server." >&2
        exit 2 ;;
esac

ASSETS="${AMINETXDUO_ASSETS:-$HOME/amiga-assets}"
AHOST="$ASSETS/apps/adig/ahost-68000"
ADIG="$ASSETS/apps/adig/adig-68000"
TOOLS="$ROOT/$BUILD/src/tools"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"

# Exit 2 is "this machine cannot make the assertion", which is not the same as
# the assertion failing.  Every missing piece is named.
for f in "$AHOST" "$ADIG" "$TOOLS/ToolsSmoke" "$TOOLS/AddNetInterface" "$BSD"; do
    [ -f "$f" ] || { echo "ahost=skipped reason=missing file=$f" >&2; exit 2; }
done

. "$ROOT/tools/sana2-stage.sh"

# sana2_stage_driver() stays SILENT when a2065.device is missing -- every other
# board warns -- so a run without it reaches the guest as "There is no
# a2065.device on this machine" and nothing before that says why.  Find it here
# and refuse, the way tests/tools/run-dns.sh does.
A2065="${AMINETXDUO_A2065:-}"
if [ -z "$A2065" ]; then
    for candidate in \
        "$ROOT/build/a2065.device" \
        "$HOME/amiga-os-src/os-source/other_networking/sana2/bin/devs/a2065.device"
    do
        [ -f "$candidate" ] && { A2065="$candidate"; break; }
    done
fi
if [ "$BOARD" = a2065 ] && { [ -z "$A2065" ] || [ ! -f "$A2065" ]; }; then
    echo "ahost=skipped reason=no_a2065 set=AMINETXDUO_A2065" >&2
    exit 2
fi

# WHAT THE ANSWER SHOULD BE, decided on this host before the guest runs.  A
# guest that prints an address nobody can corroborate proves only that it
# printed something.
mapfile -t EXPECT < <(getent ahostsv4 "$NAME" 2>/dev/null | awk '{print $1}' | sort -u)
if [ "${#EXPECT[@]}" = 0 ]; then
    echo "ahost=skipped reason=host_cannot_resolve name=$NAME" >&2
    exit 2
fi
echo "==> $NAME is ${EXPECT[*]} from this host"

STAGE="$ROOT/build/ahost-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$BSD" "$STAGE/libs/bsdsocket.library"
cp "$TOOLS/AddNetInterface" "$STAGE/AddNetInterface"
[ "$BOARD" = a2065 ] && cp "$A2065" "$STAGE/devs/a2065.device"
cp "$AHOST" "$STAGE/ahost"
cp "$ADIG"  "$STAGE/adig"

cat > "$STAGE/devs/NetInterfaces/eth0" <<IFEOF
DEVICE=$(sana2_driver_for "$BOARD")
UNIT=0
CONFIGURE=DHCP
IFEOF

# A NAMESERVER LINE, WHICH THE SHARED FIXTURE DELIBERATELY HAS NOT GOT.
#
# tests/netstack/devs/Internet/name_resolution ships with no nameserver: our
# own resolver takes its servers from the DHCP lease, and a file server would
# outrank the leased one and cost a second per lookup on thirty harnesses.
#
# c-ares does its own DNS and reads this file for servers.  With no line in it
# ahost has nowhere to ask and exits 3 having printed nothing, which is what
# the first run of this arm showed.  That is worth knowing on its own -- a
# third-party resolver on a DHCP-only machine cannot see the leased servers --
# but it is not what this arm measures, so the server is named here and the
# question is left where it belongs, in the interop notes.
printf 'nameserver %s\n' "$STATIC_DNS" >> "$STAGE/devs/Internet/name_resolution"

sana2_stage "$BOARD" "$STAGE/devs"

# `ahost <name>' forward, `ahost <address>' reverse, then adig over UDP and
# over TCP.  The TCP query is the point of running adig at all: our own
# commands never ask a name server over TCP.
cat > "$STAGE/commands.txt" <<EOF
SYS:AddNetInterface eth0
SYS:ahost $NAME
SYS:ahost ${EXPECT[0]}
SYS:adig @$STATIC_DNS $NAME A
SYS:adig @$STATIC_DNS $NAME A +tcp
EOF

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-ahost}"

set +e
HD="$ROOT/build/amiberry-testhd-$AMINETXDUO_RUN_TAG"
echo "==> booting $MODEL under Amiberry, $BOARD on $IFACE"
"$ROOT/tools/amiberry-run.sh" -N "$BOARD" -B "$IFACE" -m "$MODEL" \
    -t "$TIMEOUT" \
    "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs" \
    "$STAGE/AddNetInterface" "$STAGE/ahost" "$STAGE/adig"
RUN_RC=$?
set -e

REPORT="$HD/tools.txt"
[ -f "$REPORT" ] || {
    echo "ahost=FAIL reason=no_transcript rc=$RUN_RC" >&2
    echo "The guest wrote no $REPORT." >&2
    exit 1
}

echo
echo "==================== what the commands printed ====================="
cat "$REPORT"
echo "==================================================================="
echo

FAILED=0
CHECKS=0
fail() { echo "FAIL: $*" >&2; FAILED=$((FAILED + 1)); CHECKS=$((CHECKS + 1)); }
pass() { echo "  ok: $*"; CHECKS=$((CHECKS + 1)); }

# ToolsSmoke brackets every command:
#
#   ===== SYS:<command> =====
#   ...whatever it printed...
#   ----- rc <n>, <ms> ms, free <bytes> -----
#
# Read inside those brackets and nowhere else.  The first version of this
# grepped the WHOLE transcript for an IPv4 address and called it a resolved
# name -- and passed on a run whose network never came up, because the address
# it matched was the one this script had put in the `ahost <addr>' command
# line.  A check that can be satisfied by its own input is not a check.
block() {
    awk -v c="===== SYS:$1 =====" '
        $0 == c        { inb = 1; next }
        inb && /^----- rc / { exit }
        inb            { print }' "$REPORT"
}
block_rc() {
    awk -v c="===== SYS:$1 =====" '
        $0 == c              { inb = 1; next }
        inb && /^----- rc /   { sub(/^----- rc /, ""); sub(/,.*/, ""); print; exit }
    ' "$REPORT"
}

# The interface first: everything below is meaningless without it, and a
# resolver that answers with the network down is answering from a cache.
IF_RC=$(block_rc "AddNetInterface eth0")
if [ "${IF_RC:-x}" = 0 ]; then
    pass "AddNetInterface brought eth0 up (rc 0)"
else
    fail "AddNetInterface returned rc ${IF_RC:-none}: $(block "AddNetInterface eth0" | tr '\n' ' ')"
fi

RUNS=$(grep -c "^===== SYS:ahost $NAME =====" "$REPORT" || true)
[ "$RUNS" = 1 ] && pass "the machine booted once and ran ahost" \
                || fail "the ahost block appears $RUNS times, the machine reset"

# THE ADDRESS, out of ahost's own output, against what this host resolved.
GOT=$(block "ahost $NAME" | grep -oE '([0-9]{1,3}\.){3}[0-9]{1,3}' | sort -u || true)
MATCH=0
for e in "${EXPECT[@]}"; do
    printf '%s\n' "$GOT" | grep -qx "$e" && MATCH=1
done
if [ "$MATCH" = 1 ]; then
    pass "ahost resolved $NAME to an address this host also returns"
else
    fail "ahost printed no address this host returns for $NAME" \
         "(host: ${EXPECT[*]}; ahost said: $(printf '%s' "$GOT" | tr '\n' ' ')${GOT:+})"
fi

# THE REVERSE LOOKUP, against what this host gets for the same address.
# Requiring it to succeed would be asserting something untrue: the first
# address of a name behind a CDN often has no PTR at all, and 104.20.23.154
# has none, so the guest returning rc 3 there is CORRECT.  Ask the host and
# hold the guest to the same answer.
if getent hosts "${EXPECT[0]}" >/dev/null 2>&1; then
    RV=$(block "ahost ${EXPECT[0]}" | grep -cvE '^[[:space:]]*$' || true)
    if [ "${RV:-0}" -gt 0 ]; then
        pass "ahost ${EXPECT[0]} answered, as this host does ($RV lines)"
    else
        fail "ahost ${EXPECT[0]} printed nothing, but this host has a PTR" \
             "($(getent hosts "${EXPECT[0]}" | head -1))"
    fi
else
    RRC=$(block_rc "ahost ${EXPECT[0]}")
    if [ "${RRC:-0}" != 0 ]; then
        pass "ahost ${EXPECT[0]} found no PTR (rc $RRC), and neither does this host"
    else
        fail "ahost ${EXPECT[0]} claims a PTR this host cannot find"
    fi
fi

for t in "A" "A +tcp"; do
    cmd="adig @$STATIC_DNS $NAME $t"
    ans=$(block "$cmd" | sed -n 's/^;; flags:.*ANSWER: \([0-9]*\).*/\1/p' | head -1)
    if [ "${ans:-0}" -gt 0 ]; then
        pass "\`$cmd' returned $ans answer record(s)"
    else
        fail "\`$cmd' returned no answer records (rc $(block_rc "$cmd"))"
    fi
done

echo
echo "ahost_checks=$CHECKS failures=$FAILED"
if [ "$FAILED" != 0 ]; then
    echo "ahost=FAIL" >&2
    exit 1
fi
echo "ahost=PASS"
