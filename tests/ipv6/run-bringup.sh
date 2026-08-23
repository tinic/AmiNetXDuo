#!/usr/bin/env bash
#
# HOW LONG DOES THE NETWORK TAKE TO BECOME USABLE, IPv6 INCLUDED.
#
#   tests/ipv6/run-bringup.sh [-B BACKEND] [-b BUILDDIR] [-m MODEL]
#                             [-t SECONDS] [-n REPS] [-w SECONDS]
#
# WHAT IT MEASURES
#
# Not how long AddNetInterface takes to return.  That command came back in
# 1.8 s once the duplicate address detection and the DHCP ARP probe were moved
# off it, and the network was still some seconds away from carrying an IPv6
# packet: the link-local address was tentative, no router had been solicited,
# and the global address a router advertisement brings did not exist yet.
#
# So the figure here is the last of those, from the stack starting:
#
#   bringup_ms = max(ipv4, ip6-global) - start
#
# where each term is a `netstack: mark <event> <ms> ms` line
# (ami_netstack_mark(), src/netstack/netstack.c):
#
#   start          the stack begins, immediately before ThreadX starts
#   ipv4           an interface has a non-zero IPv4 address (the lease)
#   ip6-linklocal  fe80:: passed duplicate address detection, so it is VALID
#                  and may be a source; also what unblocks the solicitation
#   ip6-slaac      a router advertisement formed a global address, TENTATIVE
#   ip6-global     that address passed DAD.  This is "IPv6 is usable"
#
# THE CLOCK IS THE GUEST'S
#
# ami_millis() reads the E-Clock, which advances with emulated cycles, so these
# numbers do not move when something else on the host is compiling.  The host
# wall clock in tools/serial-timestamp.py's stamped log does, and is not what
# this reads.
#
# BRIDGED, OR IT MEASURES NOTHING
#
# SLIRP answers a router solicitation itself and instantly.  The tail this
# exists to measure is a real router on a real link, so -B names a host NIC and
# amiberry-run.sh refuses the run if the backend it got was not the one asked
# for.  That needs CAP_NET_RAW on the amiberry binary.
#
# OUTPUT IS key=value, ONE BLOCK PER RUN plus a summary.  Exit 0 if every rep
# reached ip6-global inside the budget, 1 if any rep came up but over it, 3 if
# a rep never got there at all, 2 for a broken invocation.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

# Every figure below is a `netstack: mark` line off the serial port, so a build
# with the log compiled out has nothing to report and this says so before it
# spends five boots finding out.
# shellcheck source=../../tools/serial-log.sh
. "$ROOT/tools/serial-log.sh"

BACKEND="${AMINETXDUO_AMIBERRY_BACKEND:-ens18}"
BUILD="${AMINETXDUO_BUILD:-build/v6log}"
MODEL=A1200
TIMEOUT=120
REPS=5
SETTLE=25
BUDGET_MS=4000

while getopts "B:b:m:t:n:w:" opt; do
    case "$opt" in
        B) BACKEND="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        n) REPS="$OPTARG" ;;
        w) SETTLE="$OPTARG" ;;
        *) sed -n '3,6p' "$0" >&2; exit 2 ;;
    esac
done
case "$BUILD" in /*) ;; *) BUILD="$ROOT/$BUILD" ;; esac

BSD="$BUILD/src/bsdsocket/bsdsocket.library"
ADDIF="$BUILD/src/tools/AddNetInterface"
SHOW="$BUILD/src/tools/ShowNetStatus"
SMOKE="$BUILD/src/tools/ToolsSmoke"
NETSTAT="$BUILD/src/tools/netstat"
for f in "$BSD" "$ADDIF" "$SHOW" "$SMOKE" "$NETSTAT"; do
    [ -f "$f" ] || { echo "build $BUILD first: no $f" >&2; exit 2; }
done

# ToolsSmoke writes to stdout and nothing else, so every byte this harness
# reads comes from the library's AMI_INFO calls -- which compile to nothing
# unless the build has AMINETXDUO_LOG.  A build without it produced
# `result=noserial run_rc=0` on rep after rep, with a complete guest
# transcript beside it, and the harness was recorded as blocked on the rig.
serial_log_require_build "$BUILD" "tests/ipv6/run-bringup.sh"

[ -n "${AMINETXDUO_KICKSTART:-}" ] || {
    echo "No Kickstart.  Set AMINETXDUO_KICKSTART=<rom>." >&2; exit 2; }

A2065="${AMINETXDUO_A2065:-}"
if [ -z "$A2065" ]; then
    for c in "$ROOT/build/a2065.device" "$HOME/amiga-assets/devs/a2065.device"; do
        [ -f "$c" ] && { A2065="$c"; break; }
    done
fi
[ -f "${A2065:-/nonexistent}" ] || {
    echo "No a2065.device found.  Set AMINETXDUO_A2065=<path>." >&2; exit 2; }

# ------------------------------------------------------------- staging ---

STAGE="$ROOT/build/bringup-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs" "$STAGE/devs/NetInterfaces"
cp "$BSD"   "$STAGE/libs/bsdsocket.library"
cp "$A2065" "$STAGE/devs/a2065.device"
cp "$ADDIF" "$STAGE/AddNetInterface"
cp "$SHOW"  "$STAGE/ShowNetStatus"
cp "$NETSTAT" "$STAGE/netstat"

# What the installer writes, plus CONFIGURE6=AUTO.  AUTO is also what an
# interface file with no CONFIGURE6 line gets; it is spelled out because this
# test is about that path and a default is a bad thing to leave implicit here.
cat > "$STAGE/devs/NetInterfaces/eth0" <<'EOF'
DEVICE=a2065.device
UNIT=0
CONFIGURE=DHCP
CONFIGURE6=AUTO
EOF

# The wait is the point: every mark after `ipv4` lands on the IP thread after
# AddNetInterface has returned, so a guest that exits when the command does
# measures nothing.  ShowNetStatus at the end is the cross-check that what the
# marks claim is also what a shipped command reports.
cat > "$STAGE/commands.txt" <<EOF
SYS:AddNetInterface DEVS:NetInterfaces/eth0
wait $SETTLE
SYS:ShowNetStatus
SYS:netstat -h
EOF

# ------------------------------------------------------------------ run ---

# `mark <event> <ms> ms`, first occurrence only: `ipv4` fires again on every
# address change and a second interface would repeat the v6 ones.
mark_of() {
    sed -n "s/^\[INFO\] netstack: mark $2 \([0-9][0-9]*\) ms.*/\1/p" "$1" \
        | head -1
}

pass=0; over=0; missing=0
declare -a RESULTS=()

for rep in $(seq 1 "$REPS"); do
    TAG="bringup$rep"
    SERIAL="$ROOT/build/amiberry-serial-$TAG.log"
    HD="$ROOT/build/amiberry-testhd-$TAG"

    set +e
    AMINETXDUO_RUN_TAG="$TAG" "$ROOT/tools/amiberry-run.sh" \
        -N a2065 -B "$BACKEND" -m "$MODEL" -t "$TIMEOUT" \
        "$SMOKE" "$STAGE/devs" "$STAGE/libs" "$STAGE/AddNetInterface" \
        "$STAGE/ShowNetStatus" "$STAGE/netstat" "$STAGE/commands.txt" \
        > "$ROOT/build/$TAG.out" 2>&1
    rc=$?
    set -e

    if [ ! -s "$SERIAL" ]; then
        echo "rep=$rep result=noserial run_rc=$rc"
        missing=$((missing + 1))
        RESULTS+=("")
        continue
    fi

    t_start=$(mark_of "$SERIAL" start)
    t_v4=$(mark_of "$SERIAL" ipv4)
    t_ll=$(mark_of "$SERIAL" ip6-linklocal)
    t_slaac=$(mark_of "$SERIAL" ip6-slaac)
    t_glob=$(mark_of "$SERIAL" ip6-global)

    rel() {
        if [ -n "$1" ]; then echo $(( $1 - t_start )); else echo -1; fi
    }

    if [ -z "$t_start" ]; then
        echo "rep=$rep result=nomarks run_rc=$rc serial=$SERIAL"
        echo "  (no 'netstack: mark' lines: build with -DAMINETXDUO_LOG=ON -DAMINETXDUO_LOG_LEVEL=2)"
        missing=$((missing + 1))
        RESULTS+=("")
        continue
    fi

    r_v4=$(rel "$t_v4"); r_ll=$(rel "$t_ll")
    r_slaac=$(rel "$t_slaac"); r_glob=$(rel "$t_glob")

    if [ "$r_glob" -lt 0 ] || [ "$r_v4" -lt 0 ]; then
        bringup=-1
    elif [ "$r_glob" -gt "$r_v4" ]; then
        bringup=$r_glob
    else
        bringup=$r_v4
    fi

    if [ "$bringup" -lt 0 ]; then
        verdict=incomplete; missing=$((missing + 1))
    elif [ "$bringup" -le "$BUDGET_MS" ]; then
        verdict=under; pass=$((pass + 1))
    else
        verdict=over; over=$((over + 1))
    fi

    echo "rep=$rep verdict=$verdict bringup_ms=$bringup ipv4_ms=$r_v4 linklocal_ms=$r_ll slaac_ms=$r_slaac global_ms=$r_glob run_rc=$rc"
    RESULTS+=("$bringup")
done

# ------------------------------------------------------------- summary ---

n=0; sum=0; min=; max=
for v in "${RESULTS[@]}"; do
    if [ -z "$v" ] || [ "$v" -lt 0 ]; then continue; fi
    n=$((n + 1)); sum=$((sum + v))
    if [ -z "$min" ] || [ "$v" -lt "$min" ]; then min=$v; fi
    if [ -z "$max" ] || [ "$v" -gt "$max" ]; then max=$v; fi
done

if [ "$n" -eq 0 ]; then
    echo "reps=$REPS complete=0 result=fail"
    exit 3
fi

echo "reps=$REPS complete=$n mean_ms=$((sum / n)) min_ms=$min max_ms=$max budget_ms=$BUDGET_MS under=$pass over=$over incomplete=$missing"

[ "$missing" -eq 0 ] || exit 3
[ "$over" -eq 0 ] || exit 1
exit 0
