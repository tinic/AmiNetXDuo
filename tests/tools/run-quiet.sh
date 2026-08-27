#!/usr/bin/env bash
#
# WHAT QUIET MEANS, ASSERTED ONCE FOR EVERY COMMAND THAT HAS IT.
#
#   tests/tools/run-quiet.sh [-m MODEL] [-t SECONDS] [-b BUILDDIR]
#                            [-A [-N board] [-B backend]]
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

MODEL=A1200
TIMEOUT=300
BUILD="${AMINETXDUO_BUILD:-build/cm}"
RUNNER=slirp
BOARD="${AMINETXDUO_AMIBERRY_BOARD:-a2065}"
IFACE="${AMINETXDUO_AMIBERRY_BACKEND:-ens18}"

while getopts "m:t:b:AN:B:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        A) RUNNER=amiberry ;;
        N) RUNNER=amiberry; BOARD="$OPTARG" ;;
        B) RUNNER=amiberry; IFACE="$OPTARG" ;;
        *) sed -n '3,7p' "$0" >&2; exit 2 ;;
    esac
done

case "$BUILD" in /*) ;; *) BUILD="${BUILD#./}" ;; esac

TOOLS="$ROOT/$BUILD/src/tools"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"

NEEDED="ToolsSmoke AddNetInterface RemoveNetInterface ConfigureNetInterface
        ShowNetStatus GetNetStatus CheckNetConfig AddNetRoute DeleteNetRoute
        NetShutdown arp hostname ping netstat"

for t in $NEEDED; do
    [ -f "$TOOLS/$t" ] || { echo "missing $TOOLS/$t, build the tree first" >&2
                            exit 2; }
done
[ -f "$BSD" ] || { echo "missing $BSD, build the tree first" >&2; exit 2; }

if [ "$RUNNER" = amiberry ]; then
    [ -n "$(ip -o -4 addr show dev "$IFACE" 2>/dev/null)" ] || {
        echo "no IPv4 address on $IFACE; -B names the host NIC the guest \
bridges onto" >&2; exit 2; }
    GW=$(ip -o -4 route show default 2>/dev/null | awk '{ print $3; exit }')
    [ -n "$GW" ] || { echo "this host has no default route, so there is no \
gateway for the AddNetRoute case" >&2; exit 2; }

    PEER=$GW
else
    PEER=10.0.2.2
    GW=10.0.2.2
fi

echo "==> peer $PEER, gateway $GW"

A2065="${AMINETXDUO_A2065:-}"
if [ -z "$A2065" ]; then
    for candidate in "$ROOT/build/a2065.device" "$HOME/amiga-assets/devs/a2065.device"
    do
        [ -f "$candidate" ] && { A2065="$candidate"; break; }
    done
fi
[ -n "$A2065" ] && [ -f "$A2065" ] || {
    echo "No a2065.device found. Set AMINETXDUO_A2065=<path>." >&2; exit 2; }


STAGE="$ROOT/build/quiet-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$A2065" "$STAGE/devs/a2065.device"

cat > "$STAGE/devs/NetInterfaces/eth0" <<'IFEOF'
DEVICE=a2065.device
UNIT=0
CONFIGURE=DHCP
STATE=up
IFEOF

# Prepended, not written over: the staged name_resolution (tests/netstack/devs)
# is what the rest of the tree uses, and this test is about HOSTNAME alone.
printf 'HOSTNAME anxdquiet\n' > "$STAGE/devs/Internet/name_resolution.new"
cat "$STAGE/devs/Internet/name_resolution" >> "$STAGE/devs/Internet/name_resolution.new"
mv "$STAGE/devs/Internet/name_resolution.new" "$STAGE/devs/Internet/name_resolution"

if [ "$RUNNER" = amiberry ]; then
    . "$ROOT/tools/sana2-stage.sh"
    if [ -z "${AMINETXDUO_SANA2_DRIVER:-}" ] && [ "$BOARD" != a2065 ]; then
        _want=$(sana2_driver_for "$BOARD")
        _have=$(sana2_local_driver "$_want")
        [ -n "$_have" ] && [ -f "$_have" ] &&
            export AMINETXDUO_SANA2_DRIVER="$_have"
    fi
    sana2_stage "$BOARD" "$STAGE/devs"
    echo "==> $BOARD: $SANA2_DRIVER, opened as '$SANA2_DEVICE'"
fi

cp "$BSD" "$STAGE/libs/bsdsocket.library"
for t in $NEEDED; do cp "$TOOLS/$t" "$STAGE/$t"; done

cat > "$STAGE/commands.txt" <<EOF
# ---- the stack is not running yet ----
SYS:ShowNetStatus QUIET
SYS:AddNetInterface nosuch0 QUIET
SYS:CheckNetConfig
SYS:CheckNetConfig QUIET
SYS:GetNetStatus
SYS:GetNetStatus QUIET
# ---- bring it up ----
SYS:AddNetInterface eth0
SYS:RemoveNetInterface eth0
SYS:AddNetInterface eth0 QUIET
SYS:netstat -i
# ---- the commands that report ----
SYS:hostname
SYS:hostname QUIET
SYS:ping $PEER -c 2 -t 20
SYS:ping $PEER -c 2 -t 20 -q
SYS:arp
SYS:arp QUIET
# ---- the commands that act ----
SYS:AddNetRoute NETDESTINATION=192.168.77.0 GATEWAY=$GW
SYS:DeleteNetRoute DESTINATION=192.168.77.0
SYS:AddNetRoute NETDESTINATION=192.168.77.0 GATEWAY=$GW QUIET
SYS:DeleteNetRoute DESTINATION=192.168.77.0 QUIET
SYS:ConfigureNetInterface nosuch0 QUIET ADDRESS 10.99.99.99/24
SYS:RemoveNetInterface nosuch0 QUIET
# ---- and take it down ----
SYS:NetShutdown
SYS:AddNetInterface eth0 QUIET
SYS:NetShutdown QUIET
EOF

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-quiet}"
export AMINETXDUO_AMIBERRY_MAC="${AMINETXDUO_AMIBERRY_MAC:-02:41:4d:49:00:71}"

STAGED=""
for t in $NEEDED; do
    [ "$t" = ToolsSmoke ] && continue
    STAGED="$STAGED $STAGE/$t"
done

set +e
HD="$ROOT/build/amiberry-testhd-$AMINETXDUO_RUN_TAG"
if [ "$RUNNER" = amiberry ]; then
    echo "==> booting $MODEL under Amiberry, $BOARD on $IFACE"
    # shellcheck disable=SC2086
    "$ROOT/tools/amiberry-run.sh" -N "$BOARD" -B "$IFACE" -m "$MODEL" \
        -t "$TIMEOUT" "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" \
        "$STAGE/devs" "$STAGE/libs" $STAGED
else
    echo "==> booting $MODEL with the A2065 on SLIRP"
    # shellcheck disable=SC2086
    "$ROOT/tools/amiberry-run.sh" -N a2065 -m "$MODEL" -t "$TIMEOUT" \
        "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" \
        "$STAGE/libs" $STAGED
fi
RUN_RC=$?
set -e

REPORT="$HD/tools.txt"
[ -f "$REPORT" ] || { echo "FAIL: the guest wrote no $REPORT (run rc=$RUN_RC)" >&2
                      echo "quiet: 0 checks, 1 failures"; exit 1; }

echo
echo "===================== what the commands printed ====================="
cat "$REPORT"
echo "====================================================================="
echo

CHECKS=0
FAILED=0
fail() { echo "FAIL: $*" >&2; CHECKS=$((CHECKS+1)); FAILED=$((FAILED+1)); }
pass() { echo "  ok: $*";      CHECKS=$((CHECKS+1)); }

block() {
    awk -v banner="$1" -v want="$2" '
        index($0, "===== " banner " =====") == 1 { n++; if (n == want) { on = 1; next } }
        on && /^----- rc / { print; exit }
        on { print }
    ' "$REPORT"
}
body()  { block "$1" "${2:-1}" | grep -v '^----- rc ' || true; }
rc_of() { block "$1" "${2:-1}" | sed -n 's/^----- rc \([0-9-]*\),.*/\1/p'; }

trimmed() { printf '%s' "$1" | tr -d '[:space:]'; }


says_nothing() { # banner nth what
    local out; out=$(body "$1" "$2")
    if [ -z "$(trimmed "$out")" ]; then
        pass "$3"
    else
        fail "$3 -- it printed:"
        printf '%s\n' "$out" | sed 's/^/       /' >&2
    fi
}

says() { # banner nth regex what
    if body "$1" "$2" | grep -Eq -- "$3"; then
        pass "$4"
    else
        fail "$4 -- no line matched /$3/"
        body "$1" "$2" | sed 's/^/       /' >&2
    fi
}

silent_about() { # banner nth regex what
    if body "$1" "$2" | grep -Eq -- "$3"; then
        fail "$4 -- QUIET still printed it:"
        body "$1" "$2" | grep -E -- "$3" | sed 's/^/       /' >&2
    else
        pass "$4"
    fi
}

no_more_than() { # loud-banner loud-nth quiet-banner quiet-nth what
    local l q
    l=$(body "$1" "$2" | grep -c . || true)
    q=$(body "$3" "$4" | grep -c . || true)
    if [ "$q" -le "$l" ]; then
        pass "$5 ($q lines against $l)"
    else
        fail "$5: the quiet run printed MORE ($q against $l)"
        body "$3" "$4" | sed 's/^/       /' >&2
    fi
}

same_rc() { # loud-banner loud-nth quiet-banner quiet-nth what
    local l q
    l=$(rc_of "$1" "$2"); q=$(rc_of "$3" "$4")
    if [ -n "$l" ] && [ "$l" = "$q" ]; then
        pass "$5 (rc $q both ways)"
    else
        fail "$5: rc was '${l:-nothing}' loud and '${q:-nothing}' quiet"
    fi
}

want_rc() { # banner nth expected what
    local got; got=$(rc_of "$1" "$2")
    if [ "$got" = "$3" ]; then pass "$4 (rc $got)"
    else fail "$4: expected rc $3, got '${got:-nothing}'"
         block "$1" "$2" | sed 's/^/       /' >&2
    fi
}

BOOTS=$(grep -c "^===== SYS:ShowNetStatus QUIET =====" "$REPORT" || true)
if [ "$BOOTS" -eq 1 ]; then
    pass "the machine booted once, so the block numbering below holds"
else
    fail "THE MACHINE REBOOTED or the run stopped: $BOOTS runs of the first line"
fi

says "SYS:ShowNetStatus QUIET" 1 'no interface called "QUIET"' \
     "ShowNetStatus no longer takes QUIET, and says which word it choked on"
want_rc "SYS:ShowNetStatus QUIET" 1 10 "and fails rather than reporting"

says "SYS:AddNetInterface nosuch0 QUIET" 1 'no interface called "nosuch0"' \
     "AddNetInterface QUIET still reports a missing interface file"
want_rc "SYS:AddNetInterface nosuch0 QUIET" 1 20 "and still fails"

says_nothing "SYS:CheckNetConfig QUIET" 1 "CheckNetConfig QUIET prints nothing"
same_rc "SYS:CheckNetConfig" 1 "SYS:CheckNetConfig QUIET" 1 \
        "and returns what the loud run returned"

says_nothing "SYS:GetNetStatus QUIET" 1 "GetNetStatus QUIET prints nothing"
same_rc "SYS:GetNetStatus" 1 "SYS:GetNetStatus QUIET" 1 \
        "and the return code is the whole answer, unchanged"

if [ "$(body "SYS:AddNetInterface eth0" 1 | grep -c . || true)" -gt 0 ]; then
    pass "AddNetInterface without QUIET reports what came up"
else
    fail "AddNetInterface printed nothing even without QUIET, nothing below holds"
fi
says_nothing "SYS:AddNetInterface eth0 QUIET" 1 \
             "AddNetInterface QUIET brings it up and says nothing"
want_rc "SYS:AddNetInterface eth0 QUIET" 1 0 "and succeeded"
if block "SYS:netstat -i" 1 | grep -Eq '^eth0 +[0-9]+ +[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+ +up'; then
    pass "and eth0 really is up with an address, so QUIET did the work"
else
    fail "eth0 is not up after the quiet add: QUIET skipped the work"
    block "SYS:netstat -i" 1 | sed 's/^/       /' >&2
fi

if [ "$(body "SYS:hostname QUIET" 1 | grep -c . || true)" -eq 1 ]; then
    pass "hostname QUIET is exactly one line, the name"
else
    fail "hostname QUIET is not one line"
    body "SYS:hostname QUIET" 1 | sed 's/^/       /' >&2
fi
no_more_than "SYS:hostname" 1 "SYS:hostname QUIET" 1 \
             "and it is shorter than the loud run"

silent_about "SYS:ping $PEER -c 2 -t 20 -q" 1 'icmp_seq=' \
             "ping -q drops the per-packet lines"
says "SYS:ping $PEER -c 2 -t 20 -q" 1 'packets transmitted' \
     "and keeps the summary, which is the answer"
says "SYS:ping $PEER -c 2 -t 20 -q" 1 '2 received' \
     "and the packets really went, so -q did not skip the work"
says "SYS:ping $PEER -c 2 -t 20" 1 'icmp_seq=' \
     "the loud run does print them, so the case above is a difference"

silent_about "SYS:arp QUIET" 1 '^Address +Hardware address' \
             "arp QUIET drops the column heading"
says "SYS:arp" 1 '^Address +Hardware address' \
     "and the loud run prints it, so that is a difference"
no_more_than "SYS:arp" 1 "SYS:arp QUIET" 1 "and prints no more than the loud run"

says_nothing "SYS:AddNetRoute NETDESTINATION=192.168.77.0 GATEWAY=$GW QUIET" 1 \
             "AddNetRoute QUIET says nothing"
same_rc "SYS:AddNetRoute NETDESTINATION=192.168.77.0 GATEWAY=$GW" 1 \
        "SYS:AddNetRoute NETDESTINATION=192.168.77.0 GATEWAY=$GW QUIET" 1 \
        "and returns what the loud run returned"
says_nothing "SYS:DeleteNetRoute DESTINATION=192.168.77.0 QUIET" 1 \
             "DeleteNetRoute QUIET says nothing"
same_rc "SYS:DeleteNetRoute DESTINATION=192.168.77.0" 1 \
        "SYS:DeleteNetRoute DESTINATION=192.168.77.0 QUIET" 1 \
        "and returns what the loud run returned"

says "SYS:ConfigureNetInterface nosuch0 QUIET ADDRESS 10.99.99.99/24" 1 \
     'no interface called "nosuch0"' \
     "ConfigureNetInterface QUIET still reports an unknown interface"
says "SYS:RemoveNetInterface nosuch0 QUIET" 1 'no interface called "nosuch0"' \
     "RemoveNetInterface QUIET still reports an unknown interface"

says_nothing "SYS:NetShutdown QUIET" 1 "NetShutdown QUIET says nothing"
same_rc "SYS:NetShutdown" 1 "SYS:NetShutdown QUIET" 1 \
        "and returns what the loud run returned"

echo
echo "quiet: $CHECKS checks, $FAILED failures"
if [ "$FAILED" -eq 0 ] && [ "$CHECKS" -ge 30 ]; then
    echo "PASS: QUIET means the same thing in every command that has one"
    exit 0
fi
[ "$CHECKS" -ge 30 ] || echo "only $CHECKS checks ran, the floor is 30" >&2
echo "the transcript above is the whole run" >&2
exit 1
