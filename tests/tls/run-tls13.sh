#!/usr/bin/env bash
#
# Does the client actually complete a TLS 1.3 handshake?
#
#   tests/tls/run-tls13.sh [-m MODEL] [-t SECONDS] [-c CPU] [-b BUILDDIR]
#                          [-P BASE_PORT] [-B IFACE] [-H user@host]
#
# The guest is bridged onto a host NIC and the server runs on a THIRD machine.
# Both are required, and the reason is the whole history of this test.  Served
# from this machine over the emulator's SLIRP, this test failed the way a
# broken client fails: the flight after ServerHello decrypted, its four
# messages were all recognised, and then the socket was CLOSED from Certificate
# onward with no server segment to account for it, intermittently, and only
# with certificate verification on.  Bridged against a peer on another machine,
# the same binaries complete the handshake against both leaves, on both CPU
# arms, run after run.  Whatever closed that socket was between the guest and
# this host's loopback, and SLIRP is a TCP implementation inside the emulator,
# so a close it originates is invisible to any capture taken here, which is
# what "closed locally" meant.
#
# A third machine and not this one: Amiberry's bridge injects with libpcap, so
# a frame the guest addresses to its own host leaves on the wire and the switch
# does not hand it back to the port it came from.  The server is unreachable
# and nothing is logged at all.
#
# tls_handshake.c cannot answer this any more.  A 1.3 server has to sign
# CertificateVerify with RSA-PSS and nx_crypto has _nx_crypto_rsa_pss_verify
# with no matching sign, so the loopback test's server half is pinned to 1.2
# and the round it runs is a 1.2 round.  The public hosts cannot answer it
# either: they serve three-certificate chains that do not finish verifying at
# 14 MHz before the far end gives up (docs/BACKLOG.md).
#
# So the server here is tests/peer/httppeer.py with mkpki.sh's local PKI, with
# its version ceiling raised, serving a two-certificate chain whose root is in
# the trust store.  That is short enough to verify at 14 MHz, which leaves the
# 1.3 handshake as the only thing under test.
#
# Two leaves, because 1.3 authenticates them differently:
#
#   rsa2.test   RSA leaf, so CertificateVerify is rsa_pss_rsae_sha256 and the
#               PSS verify path is what carries the handshake
#   ec2.test    ECDSA P-256 leaf, ecdsa_secp256r1_sha256
#
# The verdict is the peer's own "TLS up:" line, not ours: it is the side that
# knows which version was negotiated.  A guest that quietly fell back to 1.2
# would still fetch the page, so a body alone proves nothing.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
BUILD="${AMINETXDUO_BUILD:-build/cm}"
MODEL=A1200
TIMEOUT=600
CPU=""
CLOCK=""
BASE_PORT=7300
BRIDGE="${AMINETXDUO_AMIBERRY_BACKEND:-ens18}"
PEER_HOST="${AMINETXDUO_TLS13_PEER:-}"

while getopts "m:t:c:b:P:k:B:H:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        c) CPU="$OPTARG" ;;
        k) CLOCK="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        P) BASE_PORT="$OPTARG" ;;
        B) BRIDGE="$OPTARG" ;;
        H) PEER_HOST="$OPTARG" ;;
        *) sed -n '3,10p' "$0" >&2; exit 2 ;;
    esac
done
case "$BUILD" in /*) ;; *) BUILD="$ROOT/$BUILD" ;; esac

[ -n "$PEER_HOST" ] || {
    echo "no peer: -H <user@host>, or AMINETXDUO_TLS13_PEER." >&2
    echo "This test does not run over SLIRP; see the header for what that" >&2
    echo "arrangement measured instead of the client." >&2
    exit 2; }
[ -n "$BRIDGE" ] || { echo "-B <iface> names the NIC to bridge onto" >&2; exit 2; }

export AMINETXDUO_EMU_BACKEND="$BRIDGE"

# Ask the peer for its own address rather than resolving the name here: the
# machine ssh landed on cannot be wrong about its own interfaces, and a stale
# answer points the guest at something that is not serving, which reads as a
# handshake failure.
SERVER_ADDR=$(ssh "$PEER_HOST" \
    "ip -4 -o addr show scope global 2>/dev/null | awk '{print \$4}'" \
    2>/dev/null | cut -d/ -f1 | head -1)
[ -n "$SERVER_ADDR" ] || {
    echo "$PEER_HOST reported no global IPv4 address of its own" >&2; exit 2; }
BIND=0.0.0.0

SMOKE="$BUILD/src/tools/ToolsSmoke"
FETCH="$BUILD/src/tools/fetch"
ADDIF="$BUILD/src/tools/AddNetInterface"
BSD="$BUILD/src/bsdsocket/bsdsocket.library"
TLSLIB="$BUILD/src/tlslib/tls.library"

for f in "$SMOKE" "$FETCH" "$ADDIF" "$BSD" "$TLSLIB"; do
    [ -f "$f" ] || { echo "missing $f, build the tree first" >&2; exit 2; }
done

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

# ----------------------------------------------------------------- PKI ---

PKI="$ROOT/build/peer-pki"
"$ROOT/tests/peer/mkpki.sh" "$PKI" >/dev/null
[ -f "$PKI/teststore" ] || { echo "no trust store in $PKI" >&2; exit 2; }

# ------------------------------------------------------------- staging ---

STAGE="$ROOT/build/tls13-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$A2065"         "$STAGE/devs/a2065.device"
cp "$BSD"           "$STAGE/libs/bsdsocket.library"
cp "$TLSLIB"        "$STAGE/libs/tls.library"
cp "$PKI/teststore" "$STAGE/devs/Internet/certificates"
cp "$ADDIF"         "$STAGE/AddNetInterface"
cp "$FETCH"         "$STAGE/fetch"

# AMINETXDUO_PROFILE=1 samples ONLY the transfer.  AddNetInterface below brings
# the stack up first, in its own command, so DHCP and the netstack coming up
# are not in the profile: in tls_https, which starts its own stack, that was
# 6.6 s of the run and swamped everything else.
PROFARG=""
if [ "${AMINETXDUO_PROFILE:-0}" = "1" ]; then
    PROF="$BUILD/tools/profiler/Profile"
    [ -x "$PROF" ] || { echo "build the Profile target first" >&2; exit 2; }
    cp "$PROF" "$STAGE/Profile"
    PROFARG="SYS:Profile OUT=DH0:tls.prof FOLDED=DH0:tls.folded "
fi

# fetch has no --resolve, so the names the certificates carry are pointed at
# the server's address the way an Amiga has always done it.  Rewritten rather
# than appended to: tests/netstack/devs carries its own rsa2.test line and the
# resolver takes the first match, so an appended one never wins.
grep -v -e ' rsa2\.test$' -e ' ec2\.test$' \
    "$ROOT/tests/netstack/devs/Internet/hosts" > "$STAGE/devs/Internet/hosts"
cat >> "$STAGE/devs/Internet/hosts" <<EOF
$SERVER_ADDR rsa2.test
$SERVER_ADDR ec2.test
EOF

RSA_PORT=$((BASE_PORT + 1))
EC_PORT=$((BASE_PORT + 4))

cat > "$STAGE/commands.txt" <<EOF
SYS:AddNetInterface eth0
${PROFARG}SYS:fetch https://rsa2.test:$RSA_PORT/bytes/16 TIMEOUT ${AMINETXDUO_TLS13_FETCH_TIMEOUT:-25} TO DH0:rsa.bin >DH0:a-rsa.txt
SYS:fetch https://ec2.test:$EC_PORT/bytes/16 TIMEOUT ${AMINETXDUO_TLS13_FETCH_TIMEOUT:-25} TO DH0:ec.bin >DH0:b-ec.txt
EOF

# --------------------------------------------------------------- peer ---

PEERLOG="$ROOT/build/tls13-peer.log"
rm -f "$PEERLOG"

# httppeer.py finds netpeer.py at ../tools relative to itself, so the two
# directories keep their shape on the far side.  The remote log comes back over
# the ssh channel: one file to grep, wherever the server ran.
RDIR="/tmp/tls13-peer-$BASE_PORT"
ssh "$PEER_HOST" "rm -rf $RDIR && mkdir -p $RDIR" >/dev/null
# tar over ssh and not rsync: the peer only has to be a machine with ssh and
# python3, and rsync is not installed on every one of them.  It was, on the
# machine this was written against, and the run against one without it fails
# in three seconds with `rsync: command not found` -- which the gate reports
# as `result=incomplete`, indistinguishable from a guest that never booted.
tar -C "$ROOT/tests" -cf - peer tools | ssh "$PEER_HOST" "tar -C $RDIR -xf -"
tar -C "$(dirname "$PKI")" -cf - "$(basename "$PKI")" |
    ssh "$PEER_HOST" "tar -C $RDIR -xf -"
ssh -n "$PEER_HOST" \
    "cd $RDIR && AMINETXDUO_PEER_TLS13=1 exec python3 peer/httppeer.py \
         --base-port $BASE_PORT --pki $RDIR/$(basename "$PKI") \
         --bind $BIND --advertise $SERVER_ADDR --seconds $((TIMEOUT + 120))" \
    > "$PEERLOG" 2>&1 &
PEER_PID=$!
cleanup_peer() {
    kill -TERM "$PEER_PID" 2>/dev/null || true
    ssh -n "$PEER_HOST" "pkill -f 'httppeer.py --base-port $BASE_PORT'" \
        >/dev/null 2>&1 || true
}

trap cleanup_peer EXIT INT TERM HUP

sleep 3
kill -0 "$PEER_PID" 2>/dev/null || {
    echo "the peer did not start:" >&2
    cat "$PEERLOG" 2>/dev/null >&2
    exit 2
}
grep -q "listeners" "$PEERLOG" 2>/dev/null || {
    echo "the peer has not opened its listeners:" >&2
    cat "$PEERLOG" 2>/dev/null >&2; exit 2; }
echo "==> httppeer.py on $PEER_HOST ($SERVER_ADDR), TLS 1.3 ceiling:" \
     "rsa2.test on $RSA_PORT, ec2.test on $EC_PORT"

# ---------------------------------------------------------------- run ---

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-tls13}"

CPUARG=()
[ -z "$CPU" ] || CPUARG+=(-c "$CPU")
[ -z "$CLOCK" ] || CPUARG+=(-k "$CLOCK")

set +e
"$ROOT/tools/emu-net-run.sh" -n -m "$MODEL" -t "$TIMEOUT" "${CPUARG[@]}" \
    "$SMOKE" "$STAGE/devs" "$STAGE/libs" "$STAGE/AddNetInterface" \
    "$STAGE/fetch" ${PROFARG:+"$STAGE/Profile"} "$STAGE/commands.txt"
RC=$?
set -e

# ------------------------------------------------------------- verdict ---

echo
echo "============================================================"
echo "  what the server saw"
echo "============================================================"
grep -a "TLS up:\|TLS handshake from" "$PEERLOG" 2>/dev/null | tail -6 | sed 's/^/  /' \
    || echo "  (nothing reached the peer at all)"

bad=0
count13=$(grep -ac "TLS up: TLSv1.3" "$PEERLOG" 2>/dev/null || true)
count12=$(grep -ac "TLS up: TLSv1.2" "$PEERLOG" 2>/dev/null || true)
echo
echo "  TLS 1.3 handshakes: $count13    TLS 1.2: $count12    (want 2 and 0)"

if [ "$RC" != "0" ]; then
    echo "  !! the guest run itself failed, rc=$RC"
    bad=1
fi
[ "$count13" = "2" ] || bad=1
[ "$count12" = "0" ] || bad=1

if [ "$bad" = "0" ]; then
    echo "  PASS: the client completed TLS 1.3 against both leaves"
else
    echo "  FAIL: see above; nothing here is adjusted to make it pass"
fi
exit "$bad"
