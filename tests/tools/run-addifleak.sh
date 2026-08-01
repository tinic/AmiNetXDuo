#!/usr/bin/env bash
#
# THE REGRESSION TEST FOR "AddNetInterface failed and kept the machine's memory".
#
#   tests/tools/run-addifleak.sh [-t SECONDS] [-b BUILDDIR] [-n RUNS]
#
# WHAT IT PROVES
#
#   A failed AddNetInterface gives everything back.  The command runs several
#   times against a card that is not going to work, and free memory after the
#   last one has to match free memory after the first: whatever the failure
#   costs, it has to cost it once and then stop.  AmigaOS reclaims nothing when
#   a process exits, so anything the library allocated and did not hand back is
#   gone until the machine is switched off.
#
# THE MACHINE, AND WHY THIS ONE
#
#   An A600 with 1 MB of chip RAM and a PCMCIA card, on a 68000.  That is the
#   supported floor -- README says 68000, OS 2.04, 1 MB -- and it is the machine
#   the arithmetic matters on: bsdsocket.library is around 350 KB, so a copy
#   that stays resident after a failure is better than a third of the machine.
#   On an 8 MB A1200 the same defect is invisible.
#
#   The card is cnet.device on the emulated PCMCIA slot, because THE FAILURE
#   MODE IS THE POINT.  A device that does not exist fails at OpenDevice(),
#   before the packet pool, the NX_IP and the ThreadX threads are made, so there
#   is nothing allocated yet to leak -- a run against `nosuchdevice.device`
#   passes whatever the library does with a half-built stack.  A real driver
#   that opens and then cannot bring its card up is the case that gets far
#   enough to strand one, and it is what a user with a card in the slot and no
#   cable, or a card the driver cannot initialise, actually meets.
#
# READING THE RESULT
#
#   ToolsSmoke prints `rc N, free M` after every command (src/tools/toolssmoke.c).
#   The first run pays for whatever a first open costs on this machine; the
#   difference between the LAST two is the per-failure leak, and it has to be 0.
#
#   The serial log is worth reading when this fails.  `pool = ` in it means the
#   packet pool was created before the failure, so the stack was built and
#   stranded rather than never started -- which is the difference between the
#   two things this test can catch.
#
# WHAT IT NEEDS
#
#   A Kickstart the A600 boots (AMINETXDUO_KICKSTART_A600, or the default ROM),
#   cnet.device (AMINETXDUO_CNET, or ~/amiga-assets/devs/cnet.device), and a
#   68000 Release build:
#
#     cmake -S . -B build/m68000 -DAMINETXDUO_CPU=68000 -DCMAKE_BUILD_TYPE=Release
#           -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-m68k-amigaos.cmake
#     cmake --build build/m68000 --parallel
#     tests/tools/run-addifleak.sh -b build/m68000
#
#   A 68020 build on a 68000 stops with an illegal instruction before a line of
#   ours runs, and the run then looks like a hang rather than a mistake.
#
# Amiberry only.  FS-UAE cannot be driven headless on the box with the ROMs.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

TIMEOUT=300
BUILD="${AMINETXDUO_BUILD:-build/m68000}"
RUNS=3

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
    [ -f "$f" ] || { echo "missing $f -- build $BUILD first" >&2; exit 2; }
done

# amiberry-run.sh prefers AMINETXDUO_KICKSTART_A600 over the generic one, and
# an A1200's 40.68 does not boot an A600.  ~/amiga-assets/env.sh does not export
# it, so fall back to the ROM that is in the store under its own name.
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

# ------------------------------------------------------------- staging ---

STAGE="$ROOT/build/addifleak-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs" "$STAGE/devs/NetInterfaces"
cp "$BSD"   "$STAGE/libs/bsdsocket.library"
cp "$CNET"  "$STAGE/devs/cnet.device"
cp "$ADDIF" "$STAGE/AddNetInterface"
cat > "$STAGE/devs/NetInterfaces/eth0" <<'EOF'
DEVICE=cnet.device
UNIT=0
CONFIGURE=DHCP
EOF

: > "$STAGE/commands.txt"
for _ in $(seq 1 "$RUNS"); do
    echo "SYS:AddNetInterface eth0" >> "$STAGE/commands.txt"
done

# ------------------------------------------------------------------ run ---

# 1 MB of chip and nothing else.  chipmem_size counts 512 KB units, so 2 is the
# megabyte; bogomem and fastmem are cleared because the floor is 1 MB TOTAL and
# quickstart supplies some unasked.  Appended, so a caller sweeping other
# settings keeps them.
MEM="chipmem_size=2;bogomem_size=0;fastmem_size=0"
export AMINETXDUO_AMIBERRY_EXTRA="${AMINETXDUO_AMIBERRY_EXTRA:+$AMINETXDUO_AMIBERRY_EXTRA;}$MEM"
export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-addifleak}"
HD="$ROOT/build/amiberry-testhd-$AMINETXDUO_RUN_TAG"
SER="$ROOT/build/amiberry-serial-$AMINETXDUO_RUN_TAG.log"

echo "==> booting a 1 MB A600 on a 68000, cnet.device on the PCMCIA slot"
set +e
"$ROOT/tools/amiberry-run.sh" -m A600 -c 68000 -N ne2000_pcmcia -t "$TIMEOUT" \
    "$SMOKE" "$STAGE/devs" "$STAGE/libs" "$STAGE/AddNetInterface" \
    "$STAGE/commands.txt"
RUN_RC=$?
set -e

REPORT="$HD/tools.txt"
[ -f "$REPORT" ] || {
    echo "FAIL: the guest wrote no $REPORT (run rc=$RUN_RC)" >&2
    echo "      A 68020 build on a 68000 dies before writing anything -- check $BUILD." >&2
    exit 1
}

echo
echo "===================== what the guest printed ======================="
cat "$REPORT"
echo "===================================================================="
echo

# ---------------------------------------------------------- the verdict ---

FAILED=0
pass() { echo "  ok: $*"; }
fail() { echo "FAIL: $*" >&2; FAILED=$((FAILED + 1)); }

# Every free reading, in order.
mapfile -t FREE < <(sed -n 's/.*rc -\{0,1\}[0-9]\{1,\}, free \([0-9]\{1,\}\).*/\1/p' "$REPORT")

if [ "${#FREE[@]}" -lt "$RUNS" ]; then
    fail "only ${#FREE[@]} of $RUNS runs reported free memory -- the guest stopped early"
    echo "addifleak: FAILED" >&2
    exit 1
fi
pass "all $RUNS runs reported"

# The command has to have FAILED; a leak test against a run that worked proves
# nothing, and a card that unexpectedly comes up would do exactly that.
if grep -q 'rc 0,' "$REPORT"; then
    fail "a run returned 0 -- the card came up, so nothing here was tested"
fi

FIRST="${FREE[1]}"
LAST="${FREE[$((RUNS - 1))]}"
DELTA=$(( FIRST - LAST ))
PER=$(( DELTA / (RUNS - 2) ))

echo
echo "  free after run 2:      $FIRST"
echo "  free after run $RUNS:      $LAST"
echo "  leak per failed run:   $PER bytes"
echo

if [ "$DELTA" -eq 0 ]; then
    pass "a failed AddNetInterface costs nothing the second time onward"
else
    fail "$PER bytes lost per failed AddNetInterface, and never returned"
fi

if grep -q 'pool = ' "$SER" 2>/dev/null; then
    echo "  note: the packet pool was created before the failure, so the stack"
    echo "        was built and then abandoned rather than never started."
fi

echo
if [ "$FAILED" -ne 0 ]; then
    echo "addifleak: FAILED" >&2
    exit 1
fi

echo "addifleak: PASSED"
exit 0
