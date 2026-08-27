#!/usr/bin/env bash
#
# THE CONTENDED PCMCIA SOCKET.
#
#   tests/tools/run-cardslot.sh [-m MODEL] [-t SECONDS] [-b BUILDDIR]
#                               [-B backend] [-u UNIT] [-n]
#
# WHAT IT IS FOR
#
# Two paths in src/netdev/netdev_pcmcia.c have never run.  The first is the
# refusal: OwnCard() is asked with CARDF_IFAVAILABLE so a slot somebody else
# holds is declined rather than queued, and the driver must then come away
# with no unit AND no handle left behind.  The second is the recovery, in
# netdev_device.c's netdev_try_pcmcia_open(): a later OpenDevice() claims the
# slot the probe could not get.
#
# Both need a second owner of the socket, and every lab run so far has had
# exactly one.  tests/tools/cardgrab.c is that second owner.  It takes the
# slot, proves the driver is refused twice, gives the slot back, and proves
# the next open takes it -- all in one guest process and one boot, so nothing
# here depends on the order two programs happen to be scheduled in.
#
# Output is key=value plus an exit code.  -n is the negative control: it runs
# CardGrab against a unit that is not the PCMCIA row, where the refusal has
# nothing to do with the socket, and this script must then reject the run.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

# shellcheck source=../../tools/serial-log.sh
. "$ROOT/tools/serial-log.sh"

MODEL=A1200
TIMEOUT=240
BUILD="${AMINETXDUO_BUILD:-build/cm}"
BOARD=ne2000_pcmcia
IFACE="${AMINETXDUO_AMIBERRY_BACKEND:-ens18}"
UNIT=800
NEGATIVE=0

while getopts "m:t:b:B:u:nh" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        B) IFACE="$OPTARG" ;;
        u) UNIT="$OPTARG" ;;
        n) NEGATIVE=1 ;;
        h) sed -n '3,6p' "$0"; exit 0 ;;
        *) sed -n '3,6p' "$0" >&2; exit 2 ;;
    esac
done

if [ "$NEGATIVE" = 1 ]; then
    # The A2065's unit pin.  netdev_try_pcmcia_open() refuses a unit that is
    # not a PCMCIA row before it ever reaches the socket, so the open fails
    # for a reason that has nothing to do with contention -- and the gates
    # below must not accept that as a pass.
    UNIT=600
    echo "==> NEGATIVE CONTROL: unit $UNIT is not the PCMCIA row, this run must be REJECTED"
fi

TOOLS="$ROOT/$BUILD/src/tools"
GRAB="$ROOT/$BUILD/tests/tools/CardGrab"

# shellcheck source=../../tools/sana2-stage.sh
. "$ROOT/tools/sana2-stage.sh"

ANXNET=$(anxnet_binary "$ROOT/$BUILD")
[ -n "$ANXNET" ] && [ -f "$ANXNET" ] || {
    echo "no anxnet.device in $BUILD; build the tree first" >&2; exit 2; }

for f in "$TOOLS/ToolsSmoke" "$TOOLS/CheckNetDevice" "$GRAB"; do
    [ -f "$f" ] || { echo "missing $f, build the tree first" >&2; exit 2; }
done

# ------------------------------------------------------------- staging ---

STAGE="$ROOT/build/cardslot-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$ANXNET" "$STAGE/devs/anxnet.device"

# NO NetInterfaces entry.  The whole experiment turns on nothing having
# claimed the socket before CardGrab runs, and a configured interface would
# open anxnet.device during the boot and take it.
rm -f "$STAGE/devs/NetInterfaces/"*

cp "$TOOLS/CheckNetDevice" "$STAGE/CheckNetDevice"
cp "$GRAB"                 "$STAGE/CardGrab"

cat > "$STAGE/commands.txt" <<EOF
SYS:CardGrab UNIT $UNIT
SYS:CheckNetDevice
EOF

# ------------------------------------------------------------------ run ---

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-cardslot}"

HD="$ROOT/build/amiberry-testhd-$AMINETXDUO_RUN_TAG"
SERIAL=$(serial_log_path "$AMINETXDUO_RUN_TAG")

set +e
echo "==> booting $MODEL under Amiberry, $BOARD on $IFACE"
"$ROOT/tools/amiberry-run.sh" -N "$BOARD" -B "$IFACE" -m "$MODEL" \
    -t "$TIMEOUT" \
    "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs" \
    "$STAGE/CardGrab" "$STAGE/CheckNetDevice"
RUN_RC=$?
set -e

REPORT="$HD/tools.txt"
echo "run_rc=$RUN_RC"
echo "report=$REPORT"
[ -f "$REPORT" ] || {
    echo "guest_report=absent"
    echo "RESULT=fail"
    exit 1
}
echo "guest_report=present"

echo
echo "===================== what the guest printed ====================="
cat "$REPORT"
echo "=================================================================="
echo

FAILED=0
fail() { echo "gate_fail=$1"; FAILED=1; }

kv() { # key -- last value the guest printed for it, or the empty string
    sed -n "s/^$1=//p" "$REPORT" | tail -1
}

# ---- one boot ------------------------------------------------------------
STARTS=$(grep -c "^===== SYS:CardGrab " "$REPORT" || true)
echo "boots=$STARTS"
[ "$STARTS" -eq 1 ] || fail "the machine rebooted or never reached CardGrab"

# ---- the guest reached a verdict at all ----------------------------------
GRESULT=$(kv RESULT)
echo "cardgrab_result=${GRESULT:-none}"
case "$GRESULT" in
    pass|fail) ;;
    skip)
        echo "cardgrab_reason=$(sed -n 's/^reason=//p' "$REPORT" | tail -1)"
        echo "RESULT=skip"
        exit 3 ;;
    *) fail "CardGrab printed no RESULT" ;;
esac

# ---- CardGrab really owned the slot --------------------------------------
OWN=$(kv owncard)
echo "owncard=${OWN:-none}"
[ "${OWN:-1}" = 0 ] || fail "CardGrab did not get the slot, so nothing was contended"

echo "card_resource_version=$(kv card_resource_version)"

# ---- the two refusals ----------------------------------------------------
for k in contended_open contended_open2; do
    v=$(kv "$k")
    echo "$k=${v:-none}"
    [ "${v:-1}" = 0 ] || fail "$k: the driver took a slot another handle owned"
done

# ---- and the claim after the release -------------------------------------
echo "released=$(kv released)"
for k in released_open reopen; do
    v=$(kv "$k")
    echo "$k=${v:-none}"
    [ "${v:-0}" = 1 ] || fail "$k: the slot was free and the driver did not claim it"
done

# ---- the guest's own count -----------------------------------------------
NCHECKS=$(kv checks)
NFAILED=$(kv failed)
echo "guest_checks=${NCHECKS:-0}"
echo "guest_failed=${NFAILED:-unknown}"
[ "${NCHECKS:-0}" -ge 4 ] || fail "CardGrab ran only ${NCHECKS:-0} checks, wanted 4"
[ "${NFAILED:-1}" = 0 ] || fail "$NFAILED guest check(s) failed"

# ---- the probe record says the refusal was the socket's --------------------
#
# ANXDIAG_PC_OWN carries OwnCard()'s answer and CheckNetDevice prints its
# sentence.  A refusal that never reached OwnCard() would not be there, which
# is exactly what the negative control produces.
if grep -q "OwnCard() refused" "$REPORT"; then
    echo "probe_saw_contention=yes"
elif grep -q "OwnCard()" "$REPORT"; then
    echo "probe_saw_contention=no"
    fail "the probe record has an OwnCard() line and it is not a refusal, so
       the open failed somewhere other than the socket"
else
    # CheckNetDevice reads the probe record the LAST claim left, and the
    # successful one at the end overwrites the refused one.  Absent is
    # therefore not a failure here; the four gates above are the assertion.
    echo "probe_saw_contention=unrecorded"
fi

# ---- the machine survived ---------------------------------------------------
#
# A slot handed back and forth is exactly where a no-MMU machine takes a
# stack fault instead of a wrong answer, so the serial log is read even
# though nothing above needs it.  serial_log_have() prints the trio and says
# whether there was anything to read; an empty log is a fact about the rig,
# not a passing assertion, which is why it is printed either way.
serial_log_have "${SERIAL:-}" "$BUILD" "a guru during the slot handover" || true
if [ -s "${SERIAL:-/nonexistent}" ] &&
   grep -qa "Software Failure\|Guru Meditation\|Exception " "$SERIAL"; then
    echo "guest_fault=yes"
    fail "the guest took a fault while the slot changed hands"
else
    echo "guest_fault=no"
fi

echo
if [ "$NEGATIVE" = 1 ]; then
    if [ "$FAILED" -ne 0 ]; then
        echo "RESULT=pass"
        echo "note=negative control rejected, the gates need the PCMCIA row"
        exit 0
    fi
    echo "RESULT=fail"
    echo "note=negative control ACCEPTED, the gates pass on a unit that is not the socket"
    exit 1
fi

if [ "$FAILED" -ne 0 ]; then
    echo "RESULT=fail"
    exit 1
fi

echo "RESULT=pass"
exit 0
