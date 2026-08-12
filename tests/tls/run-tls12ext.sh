#!/usr/bin/env bash
#
# RFC 7366 encrypt-then-MAC and RFC 7627 extended master secret, over a real
# TLS 1.2 CBC connection.
#
#   tests/tls/run-tls12ext.sh -B IFACE -H user@host [-m MODEL] [-c CPU]
#                             [-t SECONDS] [-b BUILDDIR] [-P PORT]
#
# WHY THE SERVER IS openssl s_server AND NOT tests/peer/httppeer.py
#
# Python's ssl module will negotiate both extensions and will not say whether
# it did.  `openssl s_server -trace` decodes each handshake message and names
# every extension in it, so the verdict is the SERVER's account of what the
# guest sent and what it answered -- not the guest's account of itself, which
# is the thing under test.
#
# WHAT WOULD MAKE THIS PASS WRONGLY, AND WHY IT CANNOT
#
# Both extensions are zero-length offers, so a client could emit the four
# bytes and implement neither, and the ClientHello assertion alone would pass.
# The body assertion is what closes that:
#
#   encrypt-then-MAC changes the record framing.  Once the server echoes it,
#   OpenSSL MACs the ciphertext and expects the MAC after it.  A client that
#   offered the extension and still sent MAC-then-encrypt records produces
#   "bad record mac" on the first application record, and no body arrives.
#
#   the extended master secret changes the master secret.  Once the server
#   echoes it, both sides must derive it from the handshake transcript.  A
#   client that offered it and derived the old way fails the Finished check,
#   and the handshake does not complete at all.
#
# So the pass condition is all three together: both extensions in the
# ClientHello, both echoed in the ServerHello, and the bytes arrive.
#
# TLS 1.2 with a CBC suite is forced on the server.  Encrypt-then-MAC has no
# meaning for AEAD or for TLS 1.3, which are what this client prefers, and the
# CBC suites underneath them are the ones the extension exists for.
#
# A THIRD MACHINE, bridged, never SLIRP -- for the reasons in the header of
# tests/tls/run-tls13.sh, which this follows.
#
# Output is key=value plus an exit code:
#
#   0  every assertion held
#   1  an assertion failed
#   2  usage or environment error
#   3  the guest produced no transcript, so nothing was tested
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
BUILD="${AMINETXDUO_BUILD:-build/cm}"
MODEL=A1200
TIMEOUT=600
CPU=""
CLOCK=""
PORT=7400
BRIDGE="${AMINETXDUO_AMIBERRY_BACKEND:-ens18}"
PEER_HOST="${AMINETXDUO_TLS13_PEER:-}"

while getopts "m:t:c:b:P:k:B:H:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        c) CPU="$OPTARG" ;;
        k) CLOCK="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        P) PORT="$OPTARG" ;;
        B) BRIDGE="$OPTARG" ;;
        H) PEER_HOST="$OPTARG" ;;
        *) sed -n '3,8p' "$0" >&2; exit 2 ;;
    esac
done
case "$BUILD" in /*) ;; *) BUILD="$ROOT/$BUILD" ;; esac

say() { printf '%s=%s\n' "$1" "$2"; }

[ -n "$PEER_HOST" ] || { say result environment; say reason nopeer; exit 2; }
[ -n "$BRIDGE" ]    || { say result environment; say reason nobridge; exit 2; }

export AMINETXDUO_EMU_BACKEND="$BRIDGE"

SERVER_ADDR=$(ssh "$PEER_HOST" \
    "ip -4 -o addr show scope global 2>/dev/null | awk '{print \$4}'" \
    2>/dev/null | cut -d/ -f1 | head -1)
[ -n "$SERVER_ADDR" ] || { say result environment; say reason nopeeraddr; exit 2; }

SMOKE="$BUILD/src/tools/ToolsSmoke"
FETCH="$BUILD/src/tools/fetch"
ADDIF="$BUILD/src/tools/AddNetInterface"
BSD="$BUILD/src/bsdsocket/bsdsocket.library"
TLSLIB="$BUILD/src/tlslib/tls.library"

for f in "$SMOKE" "$FETCH" "$ADDIF" "$BSD" "$TLSLIB"; do
    [ -f "$f" ] || { say result environment; say reason "missing:$f"; exit 2; }
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
[ -n "$A2065" ] && [ -f "$A2065" ] || { say result environment; say reason noa2065; exit 2; }

PKI="$ROOT/build/peer-pki"
"$ROOT/tests/peer/mkpki.sh" "$PKI" >/dev/null
[ -f "$PKI/teststore" ] || { say result environment; say reason nopki; exit 2; }

# ------------------------------------------------------------- staging ---

STAGE="$ROOT/build/tls12ext-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$A2065"         "$STAGE/devs/a2065.device"
cp "$BSD"           "$STAGE/libs/bsdsocket.library"
cp "$TLSLIB"        "$STAGE/libs/tls.library"
cp "$PKI/teststore" "$STAGE/devs/Internet/certificates"
cp "$ADDIF"         "$STAGE/AddNetInterface"
cp "$FETCH"         "$STAGE/fetch"

grep -v -e ' rsa2\.test$' "$ROOT/tests/netstack/devs/Internet/hosts" \
    > "$STAGE/devs/Internet/hosts"
echo "$SERVER_ADDR rsa2.test" >> "$STAGE/devs/Internet/hosts"

cat > "$STAGE/commands.txt" <<EOF
SYS:AddNetInterface eth0
SYS:fetch https://rsa2.test:$PORT/ TIMEOUT ${AMINETXDUO_TLS12EXT_TIMEOUT:-120} TO DH0:page.txt >DH0:a-fetch.txt
EOF

# --------------------------------------------------------------- peer ---

PEERLOG="$ROOT/build/tls12ext-peer.log"
rm -f "$PEERLOG"

RDIR="/tmp/tls12ext-peer-$PORT"
ssh "$PEER_HOST" "rm -rf $RDIR && mkdir -p $RDIR/pki" >/dev/null
# tar and not rsync: the peer only has to be a machine with ssh, and rsync is
# not installed on every one of them.
tar -C "$PKI" -cf - . | ssh "$PEER_HOST" "tar -C $RDIR/pki -xf -"

# -tls1_2 pins the version, -cipher pins CBC, -trace names the extensions, and
# -www answers any path with a page so `fetch` has a body to check.
# The trace goes to a file ON THE PEER and is fetched afterwards, line
# buffered.  Piped back over the ssh channel it was cut off wherever the
# connection ended, and block buffered it lost whatever had not filled a block
# when the server was killed -- either way a truncated trace reads as an
# extension the server never sent.
ssh -n -f "$PEER_HOST" \
    "cd $RDIR && nohup stdbuf -oL openssl s_server -accept $PORT \
        -cert pki/leaf-rsa2.cert.pem -key pki/leaf-rsa2.key.pem \
        -cert_chain pki/int-rsa-1.cert.pem \
        -tls1_2 -cipher ECDHE-RSA-AES128-SHA256 -www -trace \
        > $RDIR/trace.log 2>&1 &"

cleanup_peer() {
    ssh -n "$PEER_HOST" "pkill -f 's_server -accept $PORT'" >/dev/null 2>&1 || true
}
trap cleanup_peer EXIT INT TERM HUP

sleep 3
ssh -n "$PEER_HOST" "pgrep -f 's_server -accept $PORT' >/dev/null" || {
    say result environment; say reason peer_did_not_start
    ssh -n "$PEER_HOST" "cat $RDIR/trace.log" 2>/dev/null | sed 's/^/peer: /' >&2 || true
    exit 2; }

say peer "$SERVER_ADDR:$PORT"

# ---------------------------------------------------------------- run ---

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-tls12ext}"

CPUARG=()
[ -z "$CPU" ]   || CPUARG+=(-c "$CPU")
[ -z "$CLOCK" ] || CPUARG+=(-k "$CLOCK")

set +e
"$ROOT/tools/emu-net-run.sh" -n -m "$MODEL" -t "$TIMEOUT" "${CPUARG[@]}" \
    "$SMOKE" "$STAGE/devs" "$STAGE/libs" "$STAGE/AddNetInterface" \
    "$STAGE/fetch" "$STAGE/commands.txt" > "$ROOT/build/tls12ext-run.log" 2>&1
RUN_RC=$?
set -e

sleep 1
ssh -n "$PEER_HOST" "cat $RDIR/trace.log" > "$PEERLOG" 2>/dev/null || true

# ------------------------------------------------------------- verdict ---

HD="$ROOT/build/amiberry-testhd-$AMINETXDUO_RUN_TAG"

# The trace prints one block per handshake message.  Count the extension
# inside the ClientHello block and inside the ServerHello block separately, so
# an offer the server ignored cannot be read as a negotiation.
ext_in() {   # ext_in <message> <extension-name>
    awk -v msg="$1" -v want="$2" '
        /^    [A-Za-z]+, Length=/ { inblock = ($0 ~ ("    " msg ", Length=")) }
        inblock && index($0, "extension_type=" want "(") { found = 1 }
        END { print (found ? 1 : 0) }
    ' "$PEERLOG"
}

ch_etm=$(ext_in ClientHello encrypt_then_mac)
ch_ems=$(ext_in ClientHello extended_master_secret)
sh_etm=$(ext_in ServerHello encrypt_then_mac)
sh_ems=$(ext_in ServerHello extended_master_secret)

body=0
[ -s "$HD/page.txt" ] && body=$(wc -c < "$HD/page.txt" | tr -d ' ')


say clienthello_etm "$ch_etm"
say clienthello_ems "$ch_ems"
say serverhello_etm "$sh_etm"
say serverhello_ems "$sh_ems"
say body_bytes      "$body"
say emulator_rc     "$RUN_RC"

if [ ! -s "$HD/tools.txt" ]; then
    say result incomplete
    say reason no_transcript
    exit 3
fi

bad=0
for v in "$ch_etm" "$ch_ems" "$sh_etm" "$sh_ems"; do
    [ "$v" = 1 ] || bad=1
done
[ "$body" -gt 0 ] || bad=1

if [ "$bad" = 0 ]; then
    say result pass
    exit 0
fi

say result fail
exit 1
