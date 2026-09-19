#!/usr/bin/env bash
# THE REGRESSION TEST FOR "AddNetInterface kept the machine's memory".
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

TIMEOUT=300
BUILD="${AMINETXDUO_BUILD:-build/m68000}"
RUNS=6

while getopts "t:b:n:" opt; do
    case "$opt" in
        t) TIMEOUT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        n) RUNS="$OPTARG" ;;
        *) echo "usage: $0 [-t seconds] [-b builddir] [-n runs]" >&2; exit 2 ;;
    esac
done

[ "$RUNS" -ge 3 ] || { echo "-n needs at least 3: the first run pays the one-off cost" >&2; exit 2; }

ADDIF="$ROOT/$BUILD/src/tools/AddNetInterface"
SMOKE="$ROOT/$BUILD/src/tools/ToolsSmoke"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"
for f in "$ADDIF" "$SMOKE" "$BSD"; do
    [ -f "$f" ] || { echo "missing $f, build $BUILD first" >&2; exit 2; }
done

if [ -z "${AMINETXDUO_KICKSTART_A600:-}" ]; then
    for candidate in \
        "$HOME/amiga-assets/roms/Kickstart v2.05 r37.350 (1992)(Commodore)(A600HD)[!].rom" \
        "$HOME/amiga-assets/roms/Kickstart v3.1 r40.63 (1993)(Commodore)(A500-A600-A2000)[!].rom"
    do
        [ -f "$candidate" ] && { export AMINETXDUO_KICKSTART_A600="$candidate"; break; }
    done
fi
[ -n "${AMINETXDUO_KICKSTART_A600:-}" ] || {
    echo "No A600 Kickstart.  Set AMINETXDUO_KICKSTART_A600=<rom>." >&2
    exit 2
}

CNET="${AMINETXDUO_CNET:-}"
if [ -z "$CNET" ]; then
    for candidate in "$ROOT/build/cnet.device" "$HOME/amiga-assets/devs/cnet.device"; do
        [ -f "$candidate" ] && { CNET="$candidate"; break; }
    done
fi
[ -n "$CNET" ] && [ -f "$CNET" ] || {
    echo "No cnet.device found.  Set AMINETXDUO_CNET=<path>." >&2
    exit 2
}


STAGE="$ROOT/build/addifleak-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs" "$STAGE/devs/NetInterfaces"
cp "$BSD"   "$STAGE/libs/bsdsocket.library"
cp "$CNET"  "$STAGE/devs/cnet.device"
ADDIF_DRIVER="${AMINETXDUO_ADDIF_DRIVER:-cnet.device}"
ADDIF_CARD="${AMINETXDUO_ADDIF_CARD:-}"
if [ "$ADDIF_DRIVER" != cnet.device ]; then
    cp "${AMINETXDUO_ADDIF_DRIVER_PATH:?set AMINETXDUO_ADDIF_DRIVER_PATH}" \
       "$STAGE/devs/$ADDIF_DRIVER"
fi
cp "$ADDIF" "$STAGE/AddNetInterface"
cat > "$STAGE/devs/NetInterfaces/eth0" <<EOF
DEVICE=$ADDIF_DRIVER
${ADDIF_CARD:+CARD=$ADDIF_CARD}
UNIT=0
CONFIGURE=DHCP
STATE=down
EOF

: > "$STAGE/commands.txt"
for _ in $(seq 1 "$RUNS"); do
    echo "SYS:AddNetInterface eth0" >> "$STAGE/commands.txt"
done


MEM="chipmem_size=2;bogomem_size=0;fastmem_size=0"
export AMINETXDUO_AMIBERRY_EXTRA="${AMINETXDUO_AMIBERRY_EXTRA:+$AMINETXDUO_AMIBERRY_EXTRA;}$MEM"
export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-addifleak}"
HD="$ROOT/build/amiberry-testhd-$AMINETXDUO_RUN_TAG"

echo "==> booting an A1200 with cnet.device on the PCMCIA slot"
set +e
"$ROOT/tools/amiberry-run.sh" -N ne2000_pcmcia -t "$TIMEOUT" \
    "$SMOKE" "$STAGE/devs" "$STAGE/libs" "$STAGE/AddNetInterface" \
    "$STAGE/commands.txt"
RUN_RC=$?
set -e

REPORT="$HD/tools.txt"
[ -f "$REPORT" ] || {
    echo "FAIL: the guest wrote no $REPORT (run rc=$RUN_RC)" >&2
    echo "      A 68020 build on a 68000 dies before writing anything, check $BUILD." >&2
    exit 1
}

echo
echo "===================== what the guest printed ======================="
cat "$REPORT"
echo "===================================================================="
echo


FAILED=0
pass() { echo "  ok: $*"; }
fail() { echo "FAIL: $*" >&2; FAILED=$((FAILED + 1)); }

mapfile -t FREE < <(sed -n 's/^----- rc -\{0,1\}[0-9]\{1,\},.*, free \([0-9]\{1,\}\) .*/\1/p' "$REPORT")

if [ "${#FREE[@]}" -lt "$RUNS" ]; then
    fail "only ${#FREE[@]} of $RUNS runs reported free memory, the guest stopped early"
    echo "addifleak: FAILED" >&2
    exit 1
fi
pass "all $RUNS runs reported"

# What each run must have said.  The first AddNetInterface builds the stack
# and honours STATE=down; since 0.28.7 the next one on an interface that is
# attached but down brings it up the way Online does (CHANGELOG, 0.28.7), so
# runs 2..N report "online".  Before that entry every run rebuilt and stranded
# a stack, which is the leak this file was written to catch; the memory it
# measures is the same either way, and a run that says neither is a run that
# did not reach the stack at all.
DOWN=$(grep -c 'the network is running, and eth0 is configured down' "$REPORT" || true)
UP=$(grep -c '^eth0: online, address' "$REPORT" || true)
if [ "$DOWN" -lt 1 ]; then
    fail "the first run did not build the stack and leave eth0 down (STATE=down)"
elif [ $((DOWN + UP)) -lt "$RUNS" ]; then
    fail "only $((DOWN + UP)) of $RUNS runs reached the stack (built $DOWN," \
         "brought up $UP), so there was nothing allocated to leak on the rest"
else
    pass "run 1 built the stack and left eth0 down, $UP re-add(s) brought it up:" \
         "the states 0.28.7 defines, and the memory measured is the same"
fi

FIRST="${FREE[1]}"
LAST="${FREE[$((RUNS - 1))]}"
DELTA=$(( FIRST - LAST ))
PER=$(( DELTA / (RUNS - 2) ))

echo
echo "  free after run 2:      $FIRST"
echo "  free after run $RUNS:      $LAST"
echo "  free, runs 2..$RUNS:      ${FREE[*]:1}"
echo

# A leak is monotonic: every AddNetInterface takes its bytes and none comes
# back.  The stack's own transients are not -- a lease renewal, a neighbour
# discovery or a reply in flight holds a 4 KB packet for a while and the
# next reading has it back (259112 259112 255064 255592 259112 255104 on the
# base tree, 2026-09-19, with nothing leaking) -- so a lower last reading is
# a leak only when no reading between climbed back.  Six runs, not three,
# so a transient has room to return inside the series.
CLIMBED=0
i=1
while [ $((i + 1)) -lt "$RUNS" ]; do
    if [ "${FREE[$((i + 1))]}" -gt "${FREE[$i]}" ]; then
        CLIMBED=1
    fi
    i=$((i + 1))
done

if [ "$DELTA" -le 0 ]; then
    pass "a re-add costs nothing the second time onward"
elif [ "$CLIMBED" -eq 1 ]; then
    pass "free memory dipped $DELTA bytes and came back between runs: a" \
         "transient of the stack's, not a leak"
else
    fail "$PER bytes lost per AddNetInterface, and never returned"
fi

echo
if [ "$FAILED" -ne 0 ]; then
    echo "addifleak: FAILED" >&2
    exit 1
fi

echo "addifleak: PASSED"
exit 0
