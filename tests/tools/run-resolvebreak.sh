#!/usr/bin/env bash
# HOW LONG A NAME LOOKUP BLOCKS, AND WHETHER CTRL-C GETS IT BACK.
# The a2065.device driver is not ours to ship: point AMINETXDUO_A2065 at one,
# or drop a copy in build/a2065.device.
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

MODEL=A1200
TIMEOUT=1200
BUILD="${AMINETXDUO_BUILD:-build/cm}"
# BRIDGED, AND ONLY BRIDGED.  This used to carry two branches, `-A` picking
# between them: one passed -B and one did not, so the default was
# AMINETXDUO_RUNNER=fsuae falling through to amiberry-run.sh with no backend,
# which is SLIRP.  Nothing else differed between them.  The measurement does
# not need SLIRP -- the blackhole is 192.0.2.1 and the name is probe.invalid,
# neither of which anything on a real segment answers -- and a run over the
# emulator's own TCP/IP is not a measurement of this stack blocking.
IFACE="${AMINETXDUO_RESOLVEBREAK_IFACE:-${AMINETXDUO_AMIBERRY_BACKEND:-ens18}}"
LIBRARY=""
DELAY=5

while getopts "m:t:b:l:d:B:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        l) LIBRARY="$OPTARG" ;;
        d) DELAY="$OPTARG" ;;
        B) IFACE="$OPTARG" ;;
        *) echo "usage: $0 [-m model] [-t seconds] [-b builddir] [-l library] [-d seconds] [-B interface]" >&2; exit 2 ;;
    esac
done

TOOLS="$ROOT/$BUILD/src/tools"
PROBE="$ROOT/$BUILD/tests/tools/ResolveBreak"
BSD="${LIBRARY:-$ROOT/$BUILD/src/bsdsocket/bsdsocket.library}"

for f in "$TOOLS/ToolsSmoke" "$TOOLS/AddNetInterface" "$PROBE" "$BSD"; do
    [ -f "$f" ] || { echo "missing $f, build the tree first" >&2; exit 2; }
done

A2065="${AMINETXDUO_A2065:-$ROOT/build/a2065.device}"
[ -f "$A2065" ] || {
    echo "No a2065.device found. Set AMINETXDUO_A2065=<path>." >&2
    exit 2
}

BLACKHOLE="${AMINETXDUO_RB_BLACKHOLE:-192.0.2.1}"
NAME="${AMINETXDUO_RB_NAME:-probe.invalid}"

kv() { printf '%s=%s\n' "$1" "$2"; }
refuse() { kv reason "$1"; kv RESULT refused; exit 2; }

case "$IFACE" in
    slirp|slirp_inbound)
        refuse "slirp_measures_the_emulators_resolver_not_ours: -B <interface>" ;;
esac

# THE BLACK HOLE HAS TO BE BLACK.  Everything below is a measurement of how
# long a lookup blocks with nobody to answer it, so a segment where something
# DOES answer $BLACKHOLE:53 -- a router that intercepts port 53 is the ordinary
# case -- produces an arm 1 that returns in milliseconds and an arm 3 with
# nothing left to interrupt.  That is not a red result, it is no result, and it
# is refused here rather than reported.
if command -v dig >/dev/null 2>&1; then
    if dig +time=3 +tries=1 +short "@$BLACKHOLE" "$NAME" >/dev/null 2>&1; then
        refuse "$BLACKHOLE answers DNS on this segment, so it is not a black hole"
    fi
    kv blackhole_silent yes
else
    kv blackhole_silent unchecked
fi

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-resolvebreak}"

STAGE="$ROOT/build/resolvebreak-stage-$AMINETXDUO_RUN_TAG"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$A2065" "$STAGE/devs/a2065.device"
cp "$BSD"   "$STAGE/libs/bsdsocket.library"
cp "$TOOLS/AddNetInterface" "$STAGE/AddNetInterface"
cp "$PROBE" "$STAGE/ResolveBreak"

# IPv4 ONLY, AND THAT IS THE WHOLE PREMISE.  The probe empties the name-server
# list and puts the black hole in its place, and AddDomainNameServer() and
# RemoveDomainNameServer() take a dotted quad -- Roadshow's autodoc says
# "char *address" and this stack parses it with ami_config_parse_ip()
# (src/bsdsocket/roadshow.c:151) -- while ObtainDomainNameServerList() also
# lists the IPv6 servers a DHCPv6 lease advertised.  So on a dual-stack
# segment the probe saw the router's IPv6 resolver, asked for it to be
# removed, was told EINVAL, and left it in the list: arm 1 then reached a real
# resolver, got RCODE 3 for probe.invalid in 80 ms, and arm 3 had no lookup
# left to interrupt.  CONFIGURE6=OFF is what makes the list removable.
cat > "$STAGE/devs/NetInterfaces/eth0" <<IFEOF
DEVICE=a2065.device
UNIT=0
CONFIGURE=DHCP
CONFIGURE6=OFF
IFEOF

# The stack says WHY a lookup ended, at AMI_LOG_INFO:
#   netstack: no name server answered about '<name>'
#   netstack: '<name>' not resolved (DNS status <n>)
# and those two sentences are the difference between "the black hole worked"
# and "something answered".  Without the tier this harness could only see how
# long the call took.
# shellcheck source=../../tools/serial-log.sh
. "$ROOT/tools/serial-log.sh"
serial_log_stage_env "$STAGE" 2

cat > "$STAGE/commands.txt" <<EOF
SYS:AddNetInterface eth0
SYS:ResolveBreak $BLACKHOLE $NAME $DELAY
EOF

HD="$ROOT/build/amiberry-testhd-$AMINETXDUO_RUN_TAG"

set +e
echo "==> booting $MODEL under Amiberry, a2065 bridged on $IFACE"
"$ROOT/tools/amiberry-run.sh" -N a2065 -B "$IFACE" -m "$MODEL" \
    -t "$TIMEOUT" \
    "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs" \
    "$STAGE/env" "$STAGE/AddNetInterface" "$STAGE/ResolveBreak"
RUN_RC=$?
set -e

REPORT="$HD/tools.txt"
[ -f "$REPORT" ] || { echo "FAIL: the guest wrote no $REPORT (run rc=$RUN_RC)" >&2; exit 1; }

echo
echo "===================== what the commands printed ====================="
cat "$REPORT"
echo "====================================================================="

# ------------------------------------------------------------- the verdict ---
#
# key=value and an exit code.  The probe's own "N failure(s)" line is one of
# three things read here: the other two are the PREMISE (arm 1 must block past
# the moment arm 3 sends its break, or arm 3 interrupts nothing) and the EFFECT
# (arm 3 must come back sooner than arm 1 did, or the break did nothing).

ticks_of() { # arm-heading -- ticks the "blocked for" line under it reports
    awk -v want="$1" '
        index($0, want) { armed = 1; next }
        armed && /blocked for:/ {
            if (match($0, /\(-?[0-9]+ ticks\)/)) {
                s = substr($0, RSTART + 1, RLENGTH - 8)
                print s
            }
            exit
        }' "$REPORT"
}

SERVERS=$(grep -c "^  name server in use: " "$REPORT" || true)
LEFT=$(sed -n "s/^  name server in use: //p" "$REPORT" | tr "\n" "," | sed "s/,$//")
ARM1=$(ticks_of "1  uninterrupted")
ARM3=$(ticks_of "3  break after")
FAILURES=$(sed -n "s/^\([0-9][0-9]*\) failure(s)$/\1/p" "$REPORT" | tail -1)
BREAK_TICKS=$((DELAY * 50))

kv blackhole      "$BLACKHOLE"
kv name           "$NAME"
kv servers_in_use "${SERVERS:-0}"
kv servers        "${LEFT:-none}"
kv arm1_ticks     "${ARM1:-none}"
kv arm3_ticks     "${ARM3:-none}"
kv break_ticks    "$BREAK_TICKS"
kv probe_failures "${FAILURES:-none}"
kv run_rc         "$RUN_RC"

bad=0
note() { kv resolvebreak_fail "$1"; bad=1; }

[ "${SERVERS:-0}" = 1 ] && [ "$LEFT" = "$BLACKHOLE" ] ||
    note "only_name_server_left_should_be_${BLACKHOLE}_but_is_${LEFT:-none}"

case "$ARM1" in
    ""|*[!0-9]*) note "arm1_reported_no_tick_count" ;;
    *) [ "$ARM1" -gt "$BREAK_TICKS" ] ||
           note "arm1_blocked_${ARM1}_ticks_which_is_not_past_the_${BREAK_TICKS}_tick_break" ;;
esac

case "$ARM3" in
    ""|*[!0-9]*) note "arm3_reported_no_tick_count" ;;
    *) case "$ARM1" in
           ""|*[!0-9]*) ;;
           *) [ "$ARM3" -lt "$ARM1" ] ||
                  note "arm3_took_${ARM3}_ticks_against_arm1_${ARM1}_so_the_break_shortened_nothing" ;;
       esac ;;
esac

[ "${FAILURES:-1}" = 0 ] || note "the_probe_reported_${FAILURES:-no}_failures"

if [ "$bad" -ne 0 ]; then
    kv RESULT fail
    exit 1
fi
kv RESULT pass
