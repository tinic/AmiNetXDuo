#!/usr/bin/env bash
# THE REGRESSION TEST FOR "AddNetInterface kept the machine's memory".
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

TIMEOUT=300
BUILD="${AMINETXDUO_BUILD:-build/m68000}"
RUNS=8
TOLERANCE="${AMINETXDUO_ADDIF_LEAK_TOLERANCE:-512}"

while getopts "t:b:n:" opt; do
    case "$opt" in
        t) TIMEOUT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        n) RUNS="$OPTARG" ;;
        *) echo "usage: $0 [-t seconds] [-b builddir] [-n runs]" >&2; exit 2 ;;
    esac
done

[ "$RUNS" -ge 6 ] || { echo "-n needs at least 6: the first run pays the one-off cost and both comparison windows need samples" >&2; exit 2; }
case "$TOLERANCE" in
    ''|*[!0-9]*) echo "AMINETXDUO_ADDIF_LEAK_TOLERANCE must be a byte count" >&2; exit 2 ;;
esac

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

WINDOW=$(( (RUNS - 2) / 2 ))
EARLY_MAX=0
LATE_MAX=0

# Compare high-water marks from two windows, not individual adjacent samples.
# A packet or lease operation can temporarily hold about 4 KB, so one reading
# can dip and the next can climb even while every AddNetInterface leaks.  A
# single climb therefore proves nothing.  Requiring the later window to
# recover to the earlier window (within a small allocator tolerance) catches
# a persistent downward trend while giving transients several samples to
# leave.
i=1
while [ "$i" -le "$WINDOW" ]; do
    [ "${FREE[$i]}" -gt "$EARLY_MAX" ] && EARLY_MAX="${FREE[$i]}"
    i=$((i + 1))
done
i=$((RUNS - WINDOW))
while [ "$i" -lt "$RUNS" ]; do
    [ "${FREE[$i]}" -gt "$LATE_MAX" ] && LATE_MAX="${FREE[$i]}"
    i=$((i + 1))
done

DELTA=$(( EARLY_MAX - LATE_MAX ))

echo
echo "  early high-water mark: $EARLY_MAX"
echo "  late high-water mark:  $LATE_MAX"
echo "  free, runs 2..$RUNS:      ${FREE[*]:1}"
echo "  allowed difference:    $TOLERANCE"
echo

if [ "$DELTA" -le "$TOLERANCE" ]; then
    pass "the late samples recover to within $TOLERANCE bytes of the early samples"
else
    fail "$DELTA bytes separate the early and late high-water marks"
fi

echo
if [ "$FAILED" -ne 0 ]; then
    echo "addifleak: FAILED" >&2
    exit 1
fi

echo "addifleak: PASSED"
exit 0
