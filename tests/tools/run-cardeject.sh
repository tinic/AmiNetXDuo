#!/usr/bin/env bash
#
# THE CARD PULLED OUT AND PUT BACK.
#
#   tests/tools/run-cardeject.sh [-m MODEL] [-t SECONDS] [-b BUILDDIR]
#                                [-B backend] [-a ADDRESS] [-u UNIT] [-n]
#
# WHAT IT IS FOR
#
# netdev_pcmcia.c installs card.resource removal and insertion callbacks and
# neither had ever fired.  Amiberry could not eject a PCMCIA network card, so
# pc_on_removed(), pc_on_inserted(), the release that stays queued for the next
# card, the reconfigure, netdev_pcmcia_reattach() and the return to online were
# all code that nothing had run.
#
# It can now, through one IPC option this project adds to Amiberry:
#
#   SET_CONFIG pcmcia_inserted 0    eject
#   SET_CONFIG pcmcia_inserted 1    insert
#
# Stock Amiberry does not have that option.  It comes from
# tools/patches/amiberry/amiberry-ipc-pcmcia-inserted.diff, and this harness
# refuses rather than passes when it is missing.  The patch exists
# because the only route Amiberry ships -- LOAD_CONFIG with a config lacking
# `inserted=true` -- runs discard_prefs() + default_prefs() first and destroys
# the guest's DH0: on the way past.  Measured, and it happens even when the
# config is unchanged, so it is not about PCMCIA at all.  A guest with no
# drive cannot report what it saw.
#
# tests/tools/cardwatch.c is the witness.  It holds the unit open across the
# whole cycle, which the driver requires: the worker only brings the card back
# online while an opener is still there.  It signals through a file on DH0:
# rather than stdout, because the guest's transcript is not written until the
# command ends and this command does not end until the cycle is over.
#
# Output is key=value plus an exit code.  -n is the negative control: it waits
# out the whole budget WITHOUT ejecting anything, so the removal never comes
# and this script must reject the run.  A harness that passes when the socket
# never changed is measuring nothing.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

# shellcheck source=../../tools/serial-log.sh
. "$ROOT/tools/serial-log.sh"
# shellcheck source=../../tools/sana2-stage.sh
. "$ROOT/tools/sana2-stage.sh"

MODEL=A1200
TIMEOUT=300
BUILD="${AMINETXDUO_BUILD:-build/cm}"
BOARD=ne2000_pcmcia
IFACE="${AMINETXDUO_AMIBERRY_BACKEND:-ens18}"
ADDR="${AMINETXDUO_CARDEJECT_ADDRESS:-}"
UNIT=800
NEGATIVE=0

# How long to wait for each of the guest's three step markers.  Boot plus
# bring-up on a 68020 is the long one.
READY_WAIT="${AMINETXDUO_CARDEJECT_READY_WAIT:-200}"
SETTLE="${AMINETXDUO_CARDEJECT_SETTLE:-5}"

while getopts "m:t:b:B:a:u:nh" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        B) IFACE="$OPTARG" ;;
        a) ADDR="$OPTARG" ;;
        u) UNIT="$OPTARG" ;;
        n) NEGATIVE=1 ;;
        h) sed -n '3,6p' "$0"; exit 0 ;;
        *) sed -n '3,6p' "$0" >&2; exit 2 ;;
    esac
done

[ -n "$ADDR" ] || {
    echo "-a is not optional: the guest needs a static address on the LAN the" >&2
    echo "-B interface bridges onto, so that being online means something." >&2
    exit 2; }

[ "$NEGATIVE" = 0 ] || echo "==> NEGATIVE CONTROL: nothing is ejected, this run must be REJECTED"

TOOLS="$ROOT/$BUILD/src/tools"
WATCH="$ROOT/$BUILD/tests/tools/CardWatch"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"
ADDIF="$ROOT/$BUILD/src/tools/AddNetInterface"

ANXNET=$(anxnet_binary "$ROOT/$BUILD")
[ -n "$ANXNET" ] && [ -f "$ANXNET" ] || {
    echo "no anxnet.device in $BUILD; build the tree first" >&2; exit 2; }
for f in "$TOOLS/ToolsSmoke" "$TOOLS/CheckNetDevice" "$TOOLS/ShowNetStatus" \
         "$WATCH" "$BSD" "$ADDIF"; do
    [ -f "$f" ] || { echo "missing $f, build the tree first" >&2; exit 2; }
done

# ----------------------------------------------------------- the emulator ---
#
# The IPC socket.  Amiberry puts it in XDG_RUNTIME_DIR and falls back to /tmp,
# and concurrent instances take amiberry_1.sock .. amiberry_9.sock -- so on a
# shared rig there is no way to tell which socket belongs to which run.  A
# runtime directory of this run's own removes the guess entirely.
IPCDIR="$ROOT/build/cardeject-xdg-${AMINETXDUO_RUN_TAG:-cardeject}"
rm -rf "$IPCDIR"; mkdir -p "$IPCDIR"; chmod 700 "$IPCDIR"
SOCK="$IPCDIR/amiberry.sock"
export XDG_RUNTIME_DIR="$IPCDIR"

# TABS, not spaces.  Amiberry's ProcessCommand() splits the line on a tab, so
# a space-joined command arrives as ONE token and comes back
# "ERROR<tab>Unknown command: SET_CONFIG PCMCIA_INSERTED 1" -- note the
# upper-casing, which is what makes that reply so hard to read.  Each argument
# to this function is one protocol field.
ipc() { # field...
    python3 - "$SOCK" "$@" <<'IPC_PY'
import socket, sys
sock = sys.argv[1]
cmd = "\t".join(sys.argv[2:])
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(10)
try:
    s.connect(sock)
    s.sendall((cmd + "\n").encode())
    reply = s.recv(4096).decode(errors="replace").strip()
    # OK or ERROR, then tab-joined fields.  Tabs become spaces so the whole
    # reply fits on one key=value line.
    print(reply.replace("\t", " "))
    # A refusal has to be an exit code as well as a word, or the harness reads
    # "Unknown option" as a successful eject.
    if not reply.startswith("OK"):
        sys.exit(2)
except OSError as e:
    print("IPCERROR %s" % e)
    sys.exit(1)
finally:
    s.close()
IPC_PY
}

# --------------------------------------------------------------- staging ---

STAGE="$ROOT/build/cardeject-stage"
rm -rf "$STAGE"; mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$ANXNET" "$STAGE/devs/anxnet.device"

cat > "$STAGE/devs/NetInterfaces/eth0" <<IFEOF
DEVICE=anxnet.device
CARD=pcmcia
UNIT=0
CONFIGURE=STATIC
ADDRESS=$ADDR
NETMASK=255.255.255.0
IFEOF

cp "$BSD"                  "$STAGE/libs/bsdsocket.library"
cp "$TOOLS/CheckNetDevice" "$STAGE/CheckNetDevice"
cp "$TOOLS/ShowNetStatus"  "$STAGE/ShowNetStatus"
cp "$ADDIF"                "$STAGE/AddNetInterface"
cp "$WATCH"                "$STAGE/CardWatch"

# AddNetInterface FIRST.  The interface file alone does not bring the unit
# online -- nothing starts the stack at boot -- and CardWatch's first step
# asks the device whether it is online.  Without this the card was never up,
# the run stopped at step 1 and nothing was ever ejected.
#
# ShowNetStatus next, so the transcript records the interface up on the card
# BEFORE anything is pulled.  CardWatch then holds the unit for the cycle.
cat > "$STAGE/commands.txt" <<EOF
SYS:AddNetInterface eth0
SYS:ShowNetStatus
SYS:CardWatch UNIT $UNIT OFFWAIT 90 ONWAIT 90 READY DH0:cardwatch.step
SYS:ShowNetStatus
SYS:CheckNetDevice
EOF

# ------------------------------------------------------------------- run ---

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-cardeject}"
HD="$ROOT/build/amiberry-testhd-$AMINETXDUO_RUN_TAG"
SERIAL=$(serial_log_path "$AMINETXDUO_RUN_TAG")
STEP="$HD/cardwatch.step"
EMULOG="$ROOT/build/cardeject-emu-$AMINETXDUO_RUN_TAG.log"
# Amiberry's OWN log, which is where write_log() goes.  The file above is only
# what amiberry-run.sh printed on stdout, and the PCMCIA lines are not in it:
# tools/amiberry-run.sh:181 names this one and every `PCMCIA: ... inserted=`
# and `PCMCIA NE2000 IO configured` line is here.
UAELOG="$ROOT/build/amiberry-$AMINETXDUO_RUN_TAG.log"

rm -f "$STEP"

echo "==> booting $MODEL under Amiberry, $BOARD on $IFACE, address $ADDR"
echo "ipc_socket=$SOCK"

set +e
"$ROOT/tools/amiberry-run.sh" -N "$BOARD" -B "$IFACE" -m "$MODEL" \
    -t "$TIMEOUT" \
    "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs" \
    "$STAGE/CardWatch" "$STAGE/CheckNetDevice" "$STAGE/ShowNetStatus" \
    "$STAGE/AddNetInterface" \
    > "$EMULOG" 2>&1 &
RUN_PID=$!
set -e

# Only ever this pid.  A bare pkill on a shared rig kills other runs.
# shellcheck disable=SC2329  # invoked by the trap below, not by name
cleanup() { kill "$RUN_PID" 2> /dev/null || true; }
trap cleanup EXIT

# Wait for the guest to say which step it is on.  Polling a file the guest
# closed is the only signal that does not depend on the transcript, which is
# not written until the command ends.
await_step() { # word seconds
    local want="$1" secs="$2" i=0
    while [ "$i" -lt "$secs" ]; do
        if [ -f "$STEP" ] && grep -qx "$want" "$STEP" 2> /dev/null; then
            return 0
        fi
        kill -0 "$RUN_PID" 2> /dev/null || return 1
        sleep 1
        i=$((i + 1))
    done
    return 1
}

EJECT_RC=x
INSERT_RC=x

if await_step eject "$READY_WAIT"; then
    echo "guest_ready_for_eject=yes"
    if [ "$NEGATIVE" = 1 ]; then
        echo "eject_sent=no"
    else
        sleep "$SETTLE"
        # NOT `OUT=$(... || true); rc=$?`.  With the `|| true` inside the
        # substitution that reads the ASSIGNMENT's status, which is always 0,
        # and the harness would report every refusal as a success.
        set +e
        OUT=$(ipc SET_CONFIG pcmcia_inserted 0)
        EJECT_RC=$?
        set -e
        echo "eject_reply=$OUT"
        echo "eject_rc=$EJECT_RC"

        if await_step insert 90; then
            echo "guest_ready_for_insert=yes"
            sleep "$SETTLE"
            set +e
            OUT=$(ipc SET_CONFIG pcmcia_inserted 1)
            INSERT_RC=$?
            set -e
            echo "insert_reply=$OUT"
            echo "insert_rc=$INSERT_RC"
        else
            echo "guest_ready_for_insert=no"
        fi
    fi
else
    echo "guest_ready_for_eject=no"
fi

wait "$RUN_PID" 2> /dev/null && RUN_RC=0 || RUN_RC=$?
trap - EXIT
echo "run_rc=$RUN_RC"

REPORT="$HD/tools.txt"
[ -f "$REPORT" ] || { echo "guest_report=absent"; echo "RESULT=fail"; exit 1; }
echo "guest_report=present"

echo
echo "===================== what the guest printed ====================="
cat "$REPORT"
echo "=================================================================="
echo

FAILED=0
fail() { echo "gate_fail=$1"; FAILED=1; }
kv() { sed -n "s/^$1=//p" "$REPORT" | tail -1; }

STARTS=$(grep -c "^===== SYS:CardWatch " "$REPORT" || true)
echo "boots=$STARTS"
[ "$STARTS" -eq 1 ] || fail "the machine rebooted or never reached CardWatch"

WRESULT=$(kv RESULT)
echo "cardwatch_result=${WRESULT:-none}"
case "$WRESULT" in
    pass|fail) ;;
    skip) echo "RESULT=skip"; exit 3 ;;
    *) fail "CardWatch printed no RESULT" ;;
esac

echo "start_online=$(kv start_online)"
[ "$(kv start_online)" = 1 ] || fail "the card was not online before the eject"

OFFEV=$(kv offline_event)
ONEV=$(kv online_event)
echo "offline_event=${OFFEV:-none}"
echo "offline_mask=$(kv offline_mask)"
echo "online_event=${ONEV:-none}"
echo "online_mask=$(kv online_mask)"
[ "${OFFEV:-0}" = 1 ] || fail "pc_on_removed did not reach the unit: no offline event"
[ "${ONEV:-0}" = 1 ] || fail "pc_on_inserted did not bring the card back: no online event"

# And the events are the right ones.  netdev_event() replies with the mask that
# was POSTED, not the part of it the request asked for, so a waiter that named
# several bits is satisfied by any one of them: an ordinary receive error posts
# ERROR|RX and used to be read here as a removal.  $10 is S2EVENT_OFFLINE and
# $08 is S2EVENT_ONLINE.
offmask=$(kv offline_mask)
onmask=$(kv online_mask)
[ $(( 0x${offmask:-0} & 0x10 )) -ne 0 ] || fail "offline_mask $offmask carries no S2EVENT_OFFLINE"
[ $(( 0x${onmask:-0} & 0x08 )) -ne 0 ] || fail "online_mask $onmask carries no S2EVENT_ONLINE"

echo "guest_checks=$(kv checks)"
echo "guest_failed=$(kv failed)"
[ "$(kv failed)" = 0 ] || fail "$(kv failed) guest check(s) failed"

# The emulator's own account, so a guest that saw nothing can be told apart
# from a socket that never changed.  These are GATES and not notes: the guest
# reporting an offline event proves the driver saw a removal, and this proves
# there was one to see.
emu_count() { # pattern -- how many times the emulator logged it
    local n
    # NOT `|| echo 0`.  grep -c prints its own 0 and exits 1 when it counts
    # nothing, so the fallback appended a SECOND line and every comparison
    # against it then failed on a two-line value.
    n=$(cat "$EMULOG" "$UAELOG" 2> /dev/null | grep -ac "$1") || n=0
    echo "$n"
}
EMU_OUT=$(emu_count "inserted=0")
EMU_IN=$(emu_count "inserted=1")
echo "emu_inserted_0=$EMU_OUT"
echo "emu_inserted_1=$EMU_IN"
echo "emu_io_configured=$(emu_count "PCMCIA NE2000 IO configured")"
if [ "$NEGATIVE" = 0 ]; then
    [ "$EMU_OUT" -ge 1 ] || fail "the emulator never logged the card leaving the socket"
    # Two: the boot insertion, and the one this harness asked for.
    [ "$EMU_IN" -ge 2 ] || fail "the emulator never logged the card going back in"
fi

# LOAD_CONFIG used to kill DH0: on the way past.  SET_CONFIG must not.
n=$(emu_count "FILESYS: was not initialized")
echo "emu_filesys_lost=$n"
[ "$n" = 0 ] || fail "the drive was torn down: this is the LOAD_CONFIG damage, so the patch is not in the binary that ran"

serial_log_have "${SERIAL:-}" "$BUILD" "a guru across the card change" || true
if [ -s "${SERIAL:-/nonexistent}" ] &&
   grep -qa "Software Failure\|Guru Meditation\|Exception " "$SERIAL"; then
    echo "guest_fault=yes"
    fail "the guest took a fault across the card change"
else
    echo "guest_fault=no"
fi

echo
if [ "$NEGATIVE" = 1 ]; then
    if [ "$FAILED" -ne 0 ]; then
        echo "RESULT=pass"
        echo "note=negative control rejected, the gates need a real removal"
        exit 0
    fi
    echo "RESULT=fail"
    echo "note=negative control ACCEPTED, the gates pass with nothing ejected"
    exit 1
fi

if [ "$FAILED" -ne 0 ]; then echo "RESULT=fail"; exit 1; fi
echo "RESULT=pass"
exit 0
