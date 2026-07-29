#!/usr/bin/env bash
#
# Hours of Fitz, with the Amiga on both ends of it.
#
#   tests/soak/run-fitz-soak.sh [-t SECONDS] [-p PHASE] [-m MODEL] [-T TAG]
#                               [-b BUILDDIR] [-f FILERS] [-s SAMPLE]
#                               [-P PORT] [-r]
#
# WHAT THIS RUNS
#
#   host   fitz-serve <scratch> PORT <p>       the wire arm's server
#   guest  FitzSoak                            everything else:
#            SYS:fitz serve DH0:localshare PORT <p+1>
#            SYS:fitz mount 10.0.2.2:<p>   FITZW:
#            SYS:fitz mount 127.0.0.1:<p+1> FITZL:
#            filers, a churner running `fitz query`, and the sampler
#
# WHY BOTH ARMS
#
# tests/endurance already covers the Amiga as a Fitz client over the wire.
# The report this comes from has the Amiga SERVING, and FS-UAE's SLIRP has no
# inbound port forwarding, so the only way to put load on `fitz serve` here is
# from inside the guest, over 127.0.0.1.  That is not the wire, and a negative
# result on the loopback arm does not clear the driver.  Running both at once
# is what makes one emulator-hour worth two.
#
# PORTS.  The default base is 17821, deliberately not Fitz's own 17711 and not
# 17712: another workstream's fitz-serve holds those, and two servers on one
# port is a failure that looks like a dropped connection.
#
# WHAT COMES BACK
#
#   build/testhd-<tag>/soak-timeline.csv   pool, memory, sockets, TCP stats
#   build/testhd-<tag>/soak-events.txt     every failure, with a full snapshot
#   build/testhd-<tag>/soak-summary.txt    totals
#   build/testhd-<tag>/soak-fitz.txt       what Fitz itself printed
#   build/serial-<tag>.log                 Fitz's ADEBUG lines, and ours
#   build/soak-<tag>-peer.log              what the host server saw
#
# Those land on the host as the run goes, not at the end, so a run killed at
# hour three is still worth reading.
#
# A RUN OF HOURS HOLDS AN EMULATOR SLOT FOR HOURS.  tools/fsuae-reap.sh kills
# any fs-uae older than 15 minutes by default and cannot tell a long tenant
# from an orphan; this script prints the reap-safe invocation.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)

SECONDS_RUN=7200
PHASE=300
MODEL=A1200
TAG="${AMINETXDUO_RUN_TAG:-fitzsoak}"
BUILD="${AMINETXDUO_BUILD:-build/cm}"
FILERS=1
SAMPLE=15
RELEASED=0
PORT="${AMINETXDUO_SOAK_PORT:-17821}"
MAXXFER=262144
MAXIO=32768

while getopts "t:p:m:T:b:f:s:P:x:r" opt; do
    case "$opt" in
        t) SECONDS_RUN="$OPTARG" ;;
        p) PHASE="$OPTARG" ;;
        m) MODEL="$OPTARG" ;;
        T) TAG="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        f) FILERS="$OPTARG" ;;
        s) SAMPLE="$OPTARG" ;;
        P) PORT="$OPTARG" ;;
        x) MAXXFER="$OPTARG" ;;
        r) RELEASED=1 ;;
        *) echo "usage: $0 [-t secs] [-p phase] [-m model] [-T tag]" \
                "[-b build] [-f filers] [-s sample] [-P port] [-x maxxfer] [-r]" >&2
           exit 2 ;;
    esac
done

SERVEPORT=$((PORT + 1))

SOAK="$ROOT/build/soak/FitzSoak"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"
UG="$ROOT/$BUILD/src/usergroup/usergroup.library"
ADDIF="$ROOT/$BUILD/src/tools/AddNetInterface"
FSERVE="$ROOT/build/endurance/fitz-serve"

for f in "$SOAK" "$FSERVE" "$BSD" "$ADDIF"; do
    [ -f "$f" ] || { echo "missing $f -- build the tree, then " \
                          "tests/soak/build-fitz-soak.sh" >&2; exit 2; }
done

if [ "$RELEASED" = "1" ]; then
    AMIGA_FITZ="$ROOT/build/endurance/fitz-release"
else
    AMIGA_FITZ="$ROOT/build/endurance/fitz-debug"
fi
[ -f "$AMIGA_FITZ" ] || { echo "missing $AMIGA_FITZ" >&2; exit 2; }

# ---- a2065.device --------------------------------------------------------

A2065="${AMINETXDUO_A2065:-}"
if [ -z "$A2065" ]; then
    for candidate in \
        "$ROOT/build/a2065.device" \
        "$HOME/amiga-os-src/os-source/other_networking/sana2/bin/devs/a2065.device"
    do
        [ -f "$candidate" ] && { A2065="$candidate"; break; }
    done
fi
[ -n "$A2065" ] && [ -f "$A2065" ] || {
    echo "No a2065.device found. Set AMINETXDUO_A2065=<path>." >&2
    exit 2
}

# ---- the host server -----------------------------------------------------

if lsof -nP -iTCP:"$PORT" -sTCP:LISTEN >/dev/null 2>&1; then
    echo "!! something already listens on $PORT -- pick another with -P," >&2
    echo "!! or a second server on one port will look like a dropped" >&2
    echo "!! connection for the whole run." >&2
    exit 2
fi

SHARE="$ROOT/build/soak-share-$TAG"
rm -rf "$SHARE"
mkdir -p "$SHARE"

PEERLOG="$ROOT/build/soak-$TAG-peer.log"
: > "$PEERLOG"

"$FSERVE" "$SHARE" PORT "$PORT" > "$PEERLOG" 2>&1 &
PEER_PID=$!

cleanup_peer() { kill -TERM "$PEER_PID" 2>/dev/null || true; }
trap cleanup_peer EXIT INT TERM HUP

sleep 1
kill -0 "$PEER_PID" 2>/dev/null || {
    echo "fitz-serve did not start:" >&2
    cat "$PEERLOG" >&2
    exit 2
}
echo "==> fitz-serve on port $PORT sharing $SHARE (pid $PEER_PID)"

# ---- stage the guest -----------------------------------------------------

STAGE="$ROOT/build/soak-stage-$TAG"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs" "$STAGE/localshare"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$A2065" "$STAGE/devs/a2065.device"
cp "$BSD" "$STAGE/libs/bsdsocket.library"
[ -f "$UG" ] && cp "$UG" "$STAGE/libs/usergroup.library"
cp "$AMIGA_FITZ" "$STAGE/fitz"
cp "$ADDIF" "$STAGE/AddNetInterface"

cat > "$STAGE/fitzsoak.cfg" <<EOF
# generated by tests/soak/run-fitz-soak.sh
seconds $SECONDS_RUN
sample $SAMPLE
phase $PHASE
filers $FILERS
maxio $MAXIO
maxxfer $MAXXFER
churnevery 30
wirehost 10.0.2.2
wireport $PORT
serveport $SERVEPORT
wirevol FITZW:
localvol FITZL:
seed 20260729
EOF

export AMINETXDUO_RUN_TAG="$TAG"

# The emulator has to outlive the workload by enough to unwind the mounts and
# write the summary; -t is a deadline on DH0:.done, not on the test.
DEADLINE=$((SECONDS_RUN + 600))

echo "==> run:  $SECONDS_RUN s workload in ${PHASE}s phases, $DEADLINE s deadline"
echo "==> fitz: $(basename "$AMIGA_FITZ"), wire $PORT, serve $SERVEPORT"
echo "==> live: tail -f $ROOT/build/testhd-$TAG/soak-events.txt"
echo "==> if anyone needs to reap while this runs: tools/fsuae-reap.sh -a $((DEADLINE / 60 + 30))"

set +e
"$ROOT/tools/fsuae-run.sh" -n -m "$MODEL" -t "$DEADLINE" \
    "$SOAK" "$STAGE/devs" "$STAGE/libs" "$STAGE/fitz" \
    "$STAGE/AddNetInterface" "$STAGE/localshare" "$STAGE/fitzsoak.cfg"
RUN_RC=$?
set -e

# ---- what came back ------------------------------------------------------

HD="$ROOT/build/testhd-$TAG"

echo
echo "=== summary ==="
[ -f "$HD/soak-summary.txt" ] && cat "$HD/soak-summary.txt" || echo "(none written)"

echo
echo "=== failures ==="
if [ -s "$HD/soak-events.txt" ]; then
    FAILS=$(grep -c " FAIL " "$HD/soak-events.txt" || true)
    STALLS=$(grep -c " STALL " "$HD/soak-events.txt" || true)
    echo "  FAIL lines:  $FAILS"
    echo "  STALL lines: $STALLS"
    grep " FAIL \| STALL " "$HD/soak-events.txt" | head -20 || true
else
    echo "  (no events file)"
fi

echo
echo "=== the first operation after each idle phase ==="
grep " POSTIDLE " "$HD/soak-events.txt" 2>/dev/null | head -30 || echo "  (none)"

echo
echo "=== Fitz's own diagnostics (serial) ==="
SER="$ROOT/build/serial-$TAG.log"
if [ -f "$SER" ]; then
    echo "  EAGAIN on a blocking socket: $(grep -c 'EAGAIN' "$SER" || true)"
    echo "  send/recv failures:          $(grep -c 'send error\|recv error\|recv maxretry' "$SER" || true)"
    echo "  relisten failures:           $(grep -c 'relisten failed' "$SER" || true)"
    echo "  refused socket deletes:      $(grep -c 'nx_tcp_socket_delete refused' "$SER" || true)"
    grep -n "relisten failed\|EAGAIN\|did not complete" "$SER" | head -20 || true
else
    echo "  (no serial log)"
fi

echo
echo "=== timeline ==="
if [ -f "$HD/soak-timeline.csv" ]; then
    python3 "$ROOT/tests/soak/soakreport.py" "$HD/soak-timeline.csv" || true
else
    echo "  (none written)"
fi

exit "$RUN_RC"
