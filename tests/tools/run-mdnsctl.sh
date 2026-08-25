#!/usr/bin/env bash
#
# TURNING THE mDNS RESPONDER ON AND OFF WHILE THE MACHINE RUNS.
#
#   tests/tools/run-mdnsctl.sh [-m MODEL] [-t SECONDS] [-b BUILDDIR]
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

MODEL=A1200
TIMEOUT=60
BUILD="${AMINETXDUO_BUILD:-build/cm}"

while getopts "m:t:b:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        *) sed -n '3,5p' "$0" >&2; exit 2 ;;
    esac
done

case "$BUILD" in /*) ;; *) BUILD="${BUILD#./}" ;; esac

TOOLS="$ROOT/$BUILD/src/tools"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"

for f in "$TOOLS/ToolsSmoke" "$TOOLS/AddNetInterface" \
         "$TOOLS/RemoveNetInterface" "$TOOLS/ConfigureNetInterface" \
         "$TOOLS/ShowNetStatus" "$TOOLS/host" "$BSD"; do
    [ -f "$f" ] || { echo "missing $f, build the tree first" >&2; exit 2; }
done

A2065="${AMINETXDUO_A2065:-}"
if [ -z "$A2065" ]; then
    for candidate in \
        "$ROOT/build/a2065.device" \
        "$HOME/amiga-assets/devs/a2065.device"
    do
        [ -f "$candidate" ] && { A2065="$candidate"; break; }
    done
fi
[ -n "$A2065" ] && [ -f "$A2065" ] || {
    echo "No a2065.device found. Set AMINETXDUO_A2065=<path>." >&2
    exit 2
}

CFG_HOSTNAME="amigatest.home.lan"
LABEL="amigatest"

# ------------------------------------------------------------- staging ---

STAGE="$ROOT/build/mdnsctl-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$A2065" "$STAGE/devs/a2065.device"

cat > "$STAGE/devs/NetInterfaces/eth0" <<'IFEOF'
DEVICE=a2065.device
UNIT=0
CONFIGURE=STATIC
ADDRESS=10.0.2.15
NETMASK=255.255.255.0
GATEWAY=10.0.2.2
IFEOF

echo "hostname $CFG_HOSTNAME" >> "$STAGE/devs/Internet/name_resolution"

cat > "$STAGE/devs/Internet/service_discovery" <<'SDEOF'
# written by tests/tools/run-mdnsctl.sh
_http._tcp    80
SDEOF

cp "$BSD"                          "$STAGE/libs/bsdsocket.library"
for t in AddNetInterface RemoveNetInterface ConfigureNetInterface \
         ShowNetStatus host; do
    cp "$TOOLS/$t" "$STAGE/$t"
done

cat > "$STAGE/commands.txt" <<EOF
SYS:AddNetInterface eth0
SYS:ShowNetStatus eth0
SYS:host $LABEL.local TIMEOUT 5
SYS:ConfigureNetInterface eth0 MDNS=YES
wait 4
SYS:ShowNetStatus eth0
SYS:host $LABEL.local TIMEOUT 5
SYS:ConfigureNetInterface eth0 MDNS=NO
wait 3
SYS:ShowNetStatus eth0
SYS:host $LABEL.local TIMEOUT 5
SYS:ConfigureNetInterface eth0 MDNS=YES
wait 4
SYS:ShowNetStatus eth0
SYS:host $LABEL.local TIMEOUT 5
SYS:RemoveNetInterface eth0 FORCE
SYS:AddNetInterface eth0
wait 2
SYS:ShowNetStatus eth0
SYS:host $LABEL.local TIMEOUT 5
EOF

# ------------------------------------------------------- the host watcher ---
WATCHLOG="$ROOT/build/mdnsctl-watch.log"
python3 "$ROOT/tests/tools/mdnswatch.py" \
    --log "$WATCHLOG" --seconds "$((TIMEOUT + 3600))" --selftest \
    > "$ROOT/build/mdnsctl-watch.out" 2>&1 &
WATCH_PID=$!
cleanup_watch() {
    [ -n "$WATCH_PID" ] && kill -TERM "$WATCH_PID" 2>/dev/null || true
}
trap cleanup_watch EXIT INT TERM HUP
sleep 2
kill -0 "$WATCH_PID" 2>/dev/null || WATCH_PID=""

# ------------------------------------------------------------------ run ---

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-mdnsctl}"
HD="$ROOT/build/amiberry-testhd-$AMINETXDUO_RUN_TAG"

STARTED=$(date +%s)
echo "==> booting $MODEL with the A2065 on SLIRP"
set +e
"$ROOT/tools/amiberry-run.sh" -N a2065 -m "$MODEL" -t "$TIMEOUT" \
    "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs" \
    "$STAGE/AddNetInterface" "$STAGE/RemoveNetInterface" \
    "$STAGE/ConfigureNetInterface" "$STAGE/ShowNetStatus" "$STAGE/host"
RUN_RC=$?
set -e
ELAPSED=$(( $(date +%s) - STARTED ))

REPORT="$HD/tools.txt"
[ -f "$REPORT" ] || { echo "FAIL: the guest wrote no $REPORT (run rc=$RUN_RC)" >&2; exit 1; }

echo
echo "===================== what the commands printed ====================="
cat "$REPORT"
echo "====================================================================="
echo

FAILED=0
fail() { echo "FAIL: $*" >&2; FAILED=1; }
pass() { echo "  ok: $*"; }
note() { echo "  --: $*"; }

BROKEN=0
infra() { echo "INFRA: $*" >&2; BROKEN=$((BROKEN + 1)); }

WANTED=$(grep -c . "$STAGE/commands.txt")
RAN=$(grep -c '^===== ' "$REPORT" || true)
if [ "$RAN" -lt "$WANTED" ]; then
    STUCK=$(sed -n "$((RAN + 1))p" "$STAGE/commands.txt")
    echo "INFRA: the guest ran $RAN of $WANTED lines in ${ELAPSED}s against a" \
         "${TIMEOUT}s ceiling." >&2
    echo "       It stopped at: ${STUCK:-<past the end of the list>}" >&2
    echo "       That command hung.  Raising -t is not the fix; the run above" \
         "measured nothing." >&2
    exit 2
fi

echo "  --: the run took ${ELAPSED}s against a ${TIMEOUT}s ceiling"

# The Nth block for a given command banner: its output and nothing else.
block() {
    awk -v banner="$1" -v want="$2" '
        index($0, "===== " banner " =====") == 1 { n++; if (n == want) { on = 1; next } }
        on && /^----- rc / { print; exit }
        on { print }
    ' "$REPORT"
}

says() { # banner nth pattern description
    if block "$1" "$2" | grep -Eq -- "$3"; then
        pass "$4"
    else
        fail "$4"
        block "$1" "$2" | sed 's/^/       /' >&2
    fi
}

denies() { # banner nth pattern description
    if block "$1" "$2" | grep -Eq -- "$3"; then
        fail "$4"
        block "$1" "$2" | sed 's/^/       /' >&2
    else
        pass "$4"
    fi
}

STATUS="SYS:ShowNetStatus eth0"
LOOKUP="SYS:host $LABEL.local TIMEOUT 5"
ON="SYS:ConfigureNetInterface eth0 MDNS=YES"
OFF="SYS:ConfigureNetInterface eth0 MDNS=NO"

# ---- one boot ------------------------------------------------------------

ADDS=$(grep -c "^===== SYS:AddNetInterface eth0 =====" "$REPORT" || true)
if [ "$ADDS" -eq 2 ]; then
    pass "the machine booted once and added the interface twice, as listed"
else
    fail "AddNetInterface ran $ADDS times, not 2: the machine reset"
fi

# ---- 1: up, and the file never asked ------------------------------------

says   "$STATUS" 1 '^ *mDNS +no$'   "1: with no MDNS= in the file, it reports off"
denies "$LOOKUP" 1 "has address"    "1: and $LABEL.local does not resolve"

# ---- 2: on --------------------------------------------------------------

says "$ON"     1 "answering .local here" "2: MDNS=YES was accepted"
says "$STATUS" 2 '^ *mDNS +yes, answering \.local$' \
                                         "2: and it now reports on"
says "$LOOKUP" 2 "^$LABEL\.local has address " \
                                         "2: and $LABEL.local resolves"

# It has to be the address the interface actually holds, not merely an answer.
says "$LOOKUP" 2 "^$LABEL\.local has address 10\.0\.2\.15" \
                                         "2: with the address eth0 carries"

# ---- 3: off, which is the case a pretending flag cannot pass -------------

says   "$OFF"    1 "no longer answering .local here" "3: MDNS=NO was accepted"
says   "$STATUS" 3 '^ *mDNS +no$'  "3: and it reports off again"
denies "$LOOKUP" 3 "has address"   "3: and $LABEL.local has STOPPED resolving"

# ---- 2b: and back on, so an off/on pair is proved as well as an off ------

says "$ON"     2 "answering .local here" "3b: MDNS=YES again was accepted"
says "$STATUS" 4 '^ *mDNS +yes, answering \.local$' \
                                         "3b: it reports on after an off/on pair"
says "$LOOKUP" 4 "^$LABEL\.local has address 10\.0\.2\.15" \
                                         "3b: and the name resolves again"

# ---- 4: removed and re-added ---------------------------------------------

says   "$STATUS" 5 '^ *mDNS +no$' \
    "4: an interface removed and re-added does not claim mDNS it was not asked for"
denies "$LOOKUP" 5 "has address" \
    "4: and $LABEL.local does not resolve after the re-add"

# ---- what another machine saw --------------------------------------------

cleanup_watch
WATCH_PID=""
sleep 1

echo
echo "=============== what the HOST's network heard ======================="
[ -f "$WATCHLOG" ] && cat "$WATCHLOG"
echo "====================================================================="

if [ -z "${WATCHLOG:-}" ] || [ ! -f "$WATCHLOG" ] ||
   grep -q "INSTRUMENT UNAVAILABLE" "$WATCHLOG"; then
    infra "the host watcher could not bind UDP 5353, so nothing off this
       machine was measured"
elif ! grep -q "the instrument SAW its own multicast" "$WATCHLOG"; then
    infra "the watcher never saw its own calibration query, so it was not
       listening and its silence means nothing"
else
    HEARD=$(grep -c "$LABEL" "$WATCHLOG" || true)
    ALIVE=$(grep "$LABEL" "$WATCHLOG" | grep -c "ttl=[1-9]" || true)
    BYE=$(grep "$LABEL" "$WATCHLOG" | grep -c "ttl=0\b" || true)

    if [ "$HEARD" -eq 0 ]; then
        infra "SLIRP relayed nothing carrying '$LABEL' onto the host's own
       network, so whether another machine could see this name was not
       measured here.  The guest-side verdict above stands on its own."
    else
        note "SLIRP relays: $HEARD message(s) naming '$LABEL' reached the
       host's real network, which is another machine seeing them"
        if [ "$ALIVE" -gt 0 ]; then
            pass "another machine saw $LABEL.local announced with a live TTL"
        else
            fail "'$LABEL' reached the host LAN but never with a live TTL"
        fi
        if [ "$BYE" -gt 0 ]; then
            pass "and saw the RFC 6762 10.1 goodbye, TTL zero, when it stopped"
        else
            note "no TTL-zero record reached the host LAN; the goodbye is sent
       250 ms after MDNS=NO and the relay may simply not have carried it"
        fi

        DUPE=$(grep -c "SRV=$LABEL\._http\._tcp\.local.*SRV=$LABEL\._http\._tcp\.local" \
               "$WATCHLOG" || true)
        if [ "$DUPE" -eq 0 ]; then
            pass "and never two SRVs for one service: an off/on pair does not
      register what service_discovery declared a second time"
        else
            fail "$DUPE announcement(s) carry the same SRV twice; the off/on
       pair registered the service again"
        fi
    fi
fi

echo
if [ "$FAILED" -ne 0 ]; then
    echo "mdnsctl: FAILED" >&2
    exit 1
fi

if [ "$BROKEN" -ne 0 ]; then
    echo "mdnsctl: PASSED on the guest, $BROKEN host-side measurement(s) not" >&2
    echo "         taken; see INFRA above" >&2
    exit 3
fi

echo "mdnsctl: PASSED"
exit 0
