#!/usr/bin/env bash
#
# AN NFS CLIENT'S WHOLE OPENING SEQUENCE, CREDENTIALS INCLUDED.
#
#   tests/tools/run-nfsprobe.sh -P peer [-B iface] [-b build] [-N board]
#                               [-m model] [-t seconds] [-a addr] [-g gw]
#                               [-p port]
#
# tests/tools/run-nfsprobe.sh asks whether a portmap reply reaches a bound UDP
# socket.  This carries the same question through to a file: GETPORT, MNT,
# LOOKUP and READ, each XDR-encoded, from a RESERVED source port, with the
# bytes read back compared against what the peer served.
#
# WHAT IT IS REALLY FOR IS THE CREDENTIALS.  Every RPC an NFS client sends
# carries AUTH_UNIX -- uid, gid and the supplementary group list -- and those
# come from usergroup.library.  No other harness in this tree makes a guest
# produce them, and usergroup.library is the library ch_nfsc could not get
# credentials out of in 0.26.5.  NfsProbe also calls getpwnam(), which is the
# vector that reads the passwd FILE that 0.26.6 taught to accept AmiTCP 4's
# own pipe-delimited records.
#
# BOTH ENDS CHECK.  The guest asserts the file's bytes; tests/tools/nfspeer.py
# parses the AUTH_UNIX body and reports the uid, gid and groups it was sent, so
# a credential this stack gets wrong fails at the peer as well as here.
#
# NO rpcbind AND NO NFS SERVER ARE INSTALLED ANYWHERE.  Binding 111 and 2049
# needs root and the question does not: nfspeer.py answers on one unprivileged
# port and the probe is told which.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)

PEERHOST="${AMINETXDUO_FITZ_PEER:-}"
PEER_IF="${AMINETXDUO_NFSPROBE_IFACE:-ens18}"
BUILD="build/cm"
BOARD="a2065"
MODEL="A1200"
TIMEOUT=180
ADDRESS="${AMINETXDUO_NFSPROBE_ADDRESS:-192.168.1.238}"
GATEWAY="${AMINETXDUO_NFSPROBE_GATEWAY:-192.168.1.1}"
NETMASK=255.255.255.0
EXPORT="/export"
FILE="payload.txt"
PORT="${AMINETXDUO_NFSPROBE_PORT:-12049}"

while getopts "P:B:b:N:m:t:a:g:p:h" opt; do
    case "$opt" in
        P) PEERHOST="$OPTARG" ;;
        B) PEER_IF="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        N) BOARD="$OPTARG" ;;
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        a) ADDRESS="$OPTARG" ;;
        g) GATEWAY="$OPTARG" ;;
        p) PORT="$OPTARG" ;;
        h) sed -n '3,8p' "$0"; exit 0 ;;
        *) sed -n '3,8p' "$0" >&2; exit 2 ;;
    esac
done

[ -n "$PEERHOST" ] || {
    echo "-P is required: the responder must run on a THIRD machine, not on" >&2
    echo "the emulator host (docs/RESEARCH.md 63)." >&2
    exit 2; }

case "$BUILD" in /*) ;; *) BUILD="$ROOT/${BUILD#./}" ;; esac
TOOLS="$BUILD/src/tools"
PROBE="$BUILD/tests/tools/NfsProbe"
BSD="$BUILD/src/bsdsocket/bsdsocket.library"
# The library this harness exists to exercise.  Staging bsdsocket
# and not this one gets "no usergroup.library" and rc=20.
UG="$BUILD/src/usergroup/usergroup.library"

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-nfsprobe}"
TAG="$AMINETXDUO_RUN_TAG"
HD="$ROOT/build/amiberry-testhd-$TAG"
REPORT="$HD/tools.txt"
OUT="$ROOT/build/nfsprobe-$TAG"

# --------------------------------------------------------------- preflight ---

for f in "$TOOLS/ToolsSmoke" "$TOOLS/AddNetInterface" "$PROBE" "$BSD" "$UG"; do
    [ -f "$f" ] || { echo "missing $f, build the tree first" >&2; exit 2; }
done

[ -n "${AMINETXDUO_KICKSTART:-}" ] || {
    echo "No Kickstart.  Set AMINETXDUO_KICKSTART=<rom>." >&2; exit 2; }

A2065="${AMINETXDUO_A2065:-}"
if [ -z "$A2065" ]; then
    for c in "$ROOT/build/a2065.device" "$HOME/amiga-assets/devs/a2065.device"; do
        [ -f "$c" ] && { A2065="$c"; break; }
    done
fi
[ -n "$A2065" ] && [ -f "$A2065" ] || {
    echo "No a2065.device found.  Set AMINETXDUO_A2065=<path>." >&2; exit 2; }

PEERNAME="${PEERHOST#*@}"
PEERADDR=$(getent ahostsv4 "$PEERNAME" 2>/dev/null | awk 'NR==1{print $1}')
[ -n "$PEERADDR" ] || case "$PEERNAME" in
    *[!0-9.]*) echo "cannot resolve $PEERNAME" >&2; exit 2 ;;
    *) PEERADDR="$PEERNAME" ;;
esac

peer_sh() { ssh -o BatchMode=yes -o ConnectTimeout=10 -n "$PEERHOST" "$@"; }

peer_sh "command -v python3 >/dev/null" || {
    echo "$PEERHOST has no python3, which is what answers the call" >&2
    exit 2; }

# THE ADDRESS HAS TO BE FREE.  Same lesson as run-cardsweep.sh: an occupied
# address loses duplicate-address detection and the arm times out saying
# nothing useful.  One of OUR guests (02:41:4d:49:*) is never a conflict.
if command -v ip > /dev/null 2>&1; then
    ip neigh del "$ADDRESS" dev "$PEER_IF" > /dev/null 2>&1 || true
    ping -c2 -W2 "$ADDRESS" > /dev/null 2>&1 || true
    sleep 2
    conflict=$(ip neigh show "$ADDRESS" 2>/dev/null \
               | grep -vi "02:41:4d:49" \
               | grep -oE "lladdr [0-9a-f:]+" | head -1 || true)
    [ -z "$conflict" ] || {
        echo "$ADDRESS is taken ($conflict); pass -a with a free one" >&2
        exit 2; }
fi

# ----------------------------------------------------------------- staging ---

STAGE="$ROOT/build/nfsprobe-stage-$TAG"
rm -rf "$STAGE" "$OUT"
mkdir -p "$STAGE/libs" "$OUT"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$A2065" "$STAGE/devs/a2065.device"

cat > "$STAGE/devs/NetInterfaces/eth0" <<IFEOF
DEVICE=a2065.device
UNIT=0
CONFIGURE=STATIC
ADDRESS=$ADDRESS
NETMASK=$NETMASK
GATEWAY=$GATEWAY
IFEOF

. "$ROOT/tools/sana2-stage.sh"
if [ -z "${AMINETXDUO_SANA2_DRIVER:-}" ] && [ "$BOARD" != a2065 ]; then
    _want=$(sana2_driver_for "$BOARD")
    _have=$(sana2_local_driver "$_want")
    [ -n "$_have" ] && [ -f "$_have" ] &&
        export AMINETXDUO_SANA2_DRIVER="$_have"
fi
sana2_stage "$BOARD" "$STAGE/devs"

cp "$BSD"   "$STAGE/libs/bsdsocket.library"
cp "$UG"    "$STAGE/libs/usergroup.library"
cp "$TOOLS/AddNetInterface" "$STAGE/AddNetInterface"
cp "$PROBE" "$STAGE/NfsProbe"

PROBECMD="SYS:NfsProbe $PEERADDR $PORT $EXPORT $FILE"
{
    echo "SYS:AddNetInterface eth0"
    echo "$PROBECMD"
} > "$STAGE/commands.txt"

# -------------------------------------------------------------- the peer -----

RTMP="/tmp/nfspeer-$TAG"
PEER_PID=""

cleanup() {
    [ -z "$PEER_PID" ] || kill "$PEER_PID" 2>/dev/null || true
    peer_sh "pkill -f '[r]pcpeer-$TAG' >/dev/null 2>&1; exit 0" || true
}
trap cleanup EXIT INT TERM HUP

peer_sh "rm -f $RTMP-*; exit 0"
scp -q "$ROOT/tests/tools/nfspeer.py" "$PEERHOST:$RTMP.py" || {
    echo "cannot copy the responder to $PEERHOST" >&2; exit 2; }

PEER_LIFE=$((TIMEOUT + 60))
ssh -o BatchMode=yes -o ConnectTimeout=10 -n "$PEERHOST" \
    "timeout $((PEER_LIFE + 30)) python3 $RTMP.py --port $PORT \
     --seconds $PEER_LIFE" > "$OUT/peer.out" 2> "$OUT/peer.err" &
PEER_PID=$!

sleep 2
kill -0 "$PEER_PID" 2>/dev/null || {
    echo "the responder died before the run started:" >&2
    cat "$OUT/peer.err" >&2; exit 2; }

# --------------------------------------------------------------------- run ---

echo "==> booting $MODEL, $BOARD bridged on $PEER_IF, guest static at $ADDRESS"
echo "==> server on $PEERHOST udp/$PORT, export $EXPORT file $FILE"
set +e
"$ROOT/tools/amiberry-run.sh" -N "$BOARD" -B "$PEER_IF" -m "$MODEL" \
    -t "$TIMEOUT" \
    "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" \
    "$STAGE/libs" "$STAGE/AddNetInterface" "$STAGE/NfsProbe"
RUN_RC=$?
set -e

sleep 2
cleanup
trap - EXIT INT TERM HUP

# ----------------------------------------------------------- what happened ---

STATUS=fail
IFACE_RC=none
CREDS=none
RESV=none
MOUNT=none
LOOKUP=none
READ=none
RESULT=none
CRED_UID=none
CRED_GID=none
NGROUPS=none

if [ -f "$REPORT" ]; then
    cp "$REPORT" "$OUT/tools.txt"
    echo
    echo "===================== what the guest printed ======================"
    cat "$REPORT"
    echo "==================================================================="

    IFACE_RC=$(awk '
        index($0, "===== SYS:AddNetInterface eth0 =====") == 1 { on = 1; next }
        on && /^----- rc / { sub(/^----- rc /, ""); sub(/,.*/, ""); print; exit }
    ' "$REPORT")
    IFACE_RC="${IFACE_RC:-none}"

    grep -q '^credentials=ok'   "$REPORT" && CREDS=ok
    grep -q '^reserved_port=ok' "$REPORT" && RESV=ok
    grep -q '^mount=ok'         "$REPORT" && MOUNT=ok
    grep -q '^lookup=ok'        "$REPORT" && LOOKUP=ok
    grep -q '^read=ok'          "$REPORT" && READ=ok
    grep -q '^RESULT=PASS'      "$REPORT" && RESULT=PASS
    CRED_UID=$(sed -n 's/^credentials=ok uid=\([0-9-]*\).*/\1/p' "$REPORT" | head -1)
    CRED_GID=$(sed -n 's/^credentials=ok uid=[0-9-]* gid=\([0-9-]*\).*/\1/p' "$REPORT" | head -1)
    NGROUPS=$(sed -n 's/.*ngroups=\([0-9-]*\).*/\1/p' "$REPORT" | head -1)
fi

# THE PEER'S OWN READING OF THE CREDENTIALS.  The guest saying it sent uid 0 and
# the server seeing uid 0 are different facts, and an XDR that encodes the
# cred body at the wrong offset satisfies the first and not the second.
PEER_CRED=$(sed -n 's/^cred_flavour=//p' "$OUT/peer.out" 2>/dev/null | head -1)
# FROM THE LINE THE PEER PRINTS WHEN THE CREDENTIALS ARRIVE, not from its
# end-of-run summary: cleanup() kills the responder, so `nfspeer_seen=` is
# usually never written and reading the count from it scored a run where the
# peer had plainly parsed AUTH_UNIX as peer_authunix=0.
# `|| true`, NOT `|| echo 0`.  grep -c PRINTS 0 and EXITS 1 when it matches
# nothing, so `|| echo 0` appends a second zero and the variable becomes the
# two-line string "0\n0" -- which is not -eq 0, and scored a run where every
# step passed as a failure.
PEER_AUTHUNIX=$(grep -c '^cred_flavour=AUTH_UNIX' "$OUT/peer.out" 2>/dev/null || true)
PEER_CALLS=$(grep -cE '^(mnt_path|lookup_name|read_off)=' "$OUT/peer.out" 2>/dev/null || true)
PEER_MISMATCH=$(grep -c '^cred_.*_mismatch=' "$OUT/peer.out" 2>/dev/null || true)
PEER_AUTHUNIX=${PEER_AUTHUNIX:-0}
PEER_CALLS=${PEER_CALLS:-0}
PEER_MISMATCH=${PEER_MISMATCH:-0}

[ "$RESULT" = PASS ] && [ "$CREDS" = ok ] && [ "$RESV" = ok ] &&
    [ "$MOUNT" = ok ] && [ "$LOOKUP" = ok ] && [ "$READ" = ok ] &&
    [ "$PEER_AUTHUNIX" -gt 0 ] && [ "$PEER_CALLS" -ge 3 ] &&
    [ "$PEER_MISMATCH" -eq 0 ] && STATUS=pass

printf 'nfsprobe: status=%s result=%s run_rc=%s iface_rc=%s creds=%s uid=%s gid=%s ngroups=%s resv=%s mount=%s lookup=%s read=%s peer_authunix=%s peer_calls=%s peer_mismatch=%s addr=%s port=%s out=%s\n' \
       "$STATUS" "$RESULT" "$RUN_RC" "$IFACE_RC" "$CREDS" "${CRED_UID:-none}" "${CRED_GID:-none}" \
       "${NGROUPS:-none}" "$RESV" "$MOUNT" "$LOOKUP" "$READ" \
       "$PEER_AUTHUNIX" "$PEER_CALLS" "$PEER_MISMATCH" "$ADDRESS" "$PORT" "$OUT"
[ -n "${PEER_CRED:-}" ] && printf 'nfsprobe: peer saw %s\n' "$PEER_CRED"

[ "$STATUS" = pass ] || exit 1
exit 0
