#!/usr/bin/env bash
# HOW LONG DOES THE NETWORK TAKE TO BECOME USABLE, IPv6 INCLUDED.
# BRIDGED, OR IT MEASURES NOTHING
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

# shellcheck source=../../tools/serial-log.sh
. "$ROOT/tools/serial-log.sh"

BACKEND="${AMINETXDUO_AMIBERRY_BACKEND:-ens18}"
# THE BUILD THAT SHIPS.  This used to default to build/v6log, a tree configured
# -DAMINETXDUO_LOG=ON -DAMINETXDUO_LOG_LEVEL=2, so the headline bring-up figure
# described a library no user ran.  The sentences are in every build now and
# the level is a runtime dial, so the ordinary tree answers and the only thing
# staged for the guest is ENV:ANXDLOGLEVEL.
BUILD="${AMINETXDUO_BUILD:-build/cm}"
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

STAGE="$ROOT/build/bringup-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs" "$STAGE/devs/NetInterfaces"
cp "$BSD"   "$STAGE/libs/bsdsocket.library"
cp "$A2065" "$STAGE/devs/a2065.device"
cp "$ADDIF" "$STAGE/AddNetInterface"
cp "$SHOW"  "$STAGE/ShowNetStatus"
cp "$NETSTAT" "$STAGE/netstat"

cat > "$STAGE/devs/NetInterfaces/eth0" <<'EOF'
DEVICE=a2065.device
UNIT=0
CONFIGURE=DHCP
CONFIGURE6=AUTO
EOF

# AMI_LOG_INFO, which is where the marks are.  ami_log_level() starts one
# level below it, so a machine with no such variable prints warnings and
# errors and nothing else -- and that is the difference between this guest and
# a user's, in full.
serial_log_stage_env "$STAGE" 2

cat > "$STAGE/commands.txt" <<EOF
SYS:AddNetInterface DEVS:NetInterfaces/eth0
wait $SETTLE
SYS:ShowNetStatus
SYS:netstat -h
EOF

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
        "$SMOKE" "$STAGE/devs" "$STAGE/libs" "$STAGE/env" \
        "$STAGE/AddNetInterface" \
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
        echo "  (no 'netstack: mark' lines: ENV:ANXDLOGLEVEL never reached the guest, or it never booted)"
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
