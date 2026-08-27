#!/usr/bin/env bash
# THE SHIPPED `fetch` COMMAND OVER HTTPS, against a server the lab owns.
#
# tests/tls/run-fetch.sh is `manual` and has to stay that way: four of its nine
# commands are third-party endpoints, so a red there can be somebody else's
# outage.  It is also the only thing that ever ran fetch's TLS path, which is
# how `2ec3fab4` shipped a client that answered
# "bsdsocket.library was built without TLS support" on every https: URL.
#
# This arm closes that: `TlsLoop SERVER ... HTTP` and `fetch` are two Shell
# processes in ONE guest talking over 127.0.0.1, against a certificate minted
# here.  No peer, no bridge traffic, no internet, no third party -- so it is a
# gate and not a weather report.
#
# Its client is `fetch` and not TlsLoop's own, which is what makes it different
# from tests/tls/run-tlsloop.sh: it exercises TLSOpen() as a shipped command
# reaches it, and grades the bytes fetch wrote to disk.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

MODEL=A1200
TIMEOUT=600
BUILD="${AMINETXDUO_BUILD:-build/cm}"
BOARD="${AMINETXDUO_AMIBERRY_BOARD:-a2065}"
IFACE="${AMINETXDUO_AMIBERRY_BACKEND:-ens18}"
PORT=7445
NAME=fetchtls.test

while getopts "m:t:b:N:B:P:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        N) BOARD="$OPTARG" ;;
        B) IFACE="$OPTARG" ;;
        P) PORT="$OPTARG" ;;
        *) echo "usage: $0 [-m model] [-t seconds] [-b builddir] [-N board] [-B backend] [-P port]" >&2; exit 2 ;;
    esac
done

case "$IFACE" in
    slirp|slirp_inbound|none)
        echo "fetchtls_backend=refused:$IFACE" >&2
        echo "This harness is bridged only.  -B names a host interface." >&2
        exit 2
        ;;
esac

TOOLS="$ROOT/$BUILD/src/tools"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"
TLS="$ROOT/$BUILD/src/tlslib/tls.library"
LOOP="$ROOT/$BUILD/tests/tls/TlsLoop"

for f in "$TOOLS/ToolsSmoke" "$TOOLS/AddNetInterface" "$TOOLS/fetch" \
         "$LOOP" "$BSD" "$TLS"; do
    [ -f "$f" ] || { echo "fetchtls_stage=missing:$f" >&2; exit 2; }
done

command -v openssl > /dev/null || { echo "fetchtls_stage=missing:openssl" >&2; exit 2; }

A2065="${AMINETXDUO_A2065:-}"
if [ -z "$A2065" ]; then
    for candidate in \
        "$ROOT/build/a2065.device" \
        "$HOME/amiga-assets/devs/a2065.device" \
        "$HOME/amiga-os-src/os-source/other_networking/sana2/bin/devs/a2065.device"
    do
        [ -f "$candidate" ] && { A2065="$candidate"; break; }
    done
fi
[ -n "$A2065" ] && [ -f "$A2065" ] || {
    echo "fetchtls_stage=missing:a2065.device" >&2
    echo "Set AMINETXDUO_A2065=<path>." >&2
    exit 2
}

# ------------------------------------------------------------------ PKI ---
#
# A root and one EC leaf signed DIRECTLY by it: src/tlslib/tls_server.c presents
# one certificate and not a chain, so an intermediate would leave fetch with a
# gap it cannot fill.  EC and not RSA because a TLS 1.3 server with an RSA key
# cannot sign CertificateVerify (see src/tlslib/tls_server.c), and this arm
# wants the version fetch will actually meet on the internet.
#
# The validity window is deliberately wide: an Amiga with no battery clock reads
# a date tls.library treats as "unknown" and one with a battery clock reads the
# host's, and the certificate has to be valid under both.

PKI="$ROOT/build/fetchtls-pki"
rm -rf "$PKI"
mkdir -p "$PKI"

NOT_BEFORE=20200101000000Z
NOT_AFTER=20450101000000Z

cat > "$PKI/leaf.cnf" <<EOF
basicConstraints=critical,CA:FALSE
keyUsage=critical,digitalSignature,keyEncipherment
extendedKeyUsage=serverAuth
subjectAltName=DNS:$NAME
subjectKeyIdentifier=hash
EOF

openssl genrsa -out "$PKI/root.key.pem" 2048 2>/dev/null
openssl req -new -x509 -key "$PKI/root.key.pem" -sha256 \
    -not_before "$NOT_BEFORE" -not_after "$NOT_AFTER" \
    -subj "/CN=FetchTls Root" -extensions v3 -config <(
        printf '[req]\ndistinguished_name=dn\n[dn]\n[v3]\n'
        printf 'basicConstraints=critical,CA:TRUE\n'
        printf 'keyUsage=critical,keyCertSign,cRLSign\n'
        printf 'subjectKeyIdentifier=hash\n') \
    -out "$PKI/root.cert.pem" 2>/dev/null

openssl ecparam -name prime256v1 -genkey -noout -out "$PKI/leaf.key.pem" 2>/dev/null
openssl ec -in "$PKI/leaf.key.pem" -outform DER -out "$PKI/leaf.key.der" 2>/dev/null
openssl req -new -key "$PKI/leaf.key.pem" -subj "/CN=$NAME" \
    -out "$PKI/leaf.csr" 2>/dev/null
openssl x509 -req -in "$PKI/leaf.csr" -sha256 \
    -CA "$PKI/root.cert.pem" -CAkey "$PKI/root.key.pem" -CAcreateserial \
    -not_before "$NOT_BEFORE" -not_after "$NOT_AFTER" \
    -extfile "$PKI/leaf.cnf" -out "$PKI/leaf.cert.pem" 2>/dev/null
openssl x509 -in "$PKI/leaf.cert.pem" -outform DER -out "$PKI/leaf.cert.der" 2>/dev/null

python3 "$ROOT/tools/mkcertstore.py" --output "$PKI/store" \
    "$PKI/root.cert.pem" > /dev/null

for f in leaf.cert.der leaf.key.der store; do
    [ -s "$PKI/$f" ] || { echo "fetchtls_pki=missing:$f" >&2; exit 2; }
done

echo "fetchtls_pki_cert=$(wc -c < "$PKI/leaf.cert.der" | tr -d ' ')"
echo "fetchtls_pki_key=$(wc -c < "$PKI/leaf.key.der" | tr -d ' ')"
echo "fetchtls_pki_store=$(wc -c < "$PKI/store" | tr -d ' ')"

# ---------------------------------------------------------------- stage ---

SELF="${AMINETXDUO_FETCHTLS_SELF:-10.79.1.2}"
MASK="${AMINETXDUO_FETCHTLS_MASK:-255.255.255.0}"

STAGE="$ROOT/build/fetchtls-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$A2065" "$STAGE/devs/a2065.device"

cat > "$STAGE/devs/NetInterfaces/eth0" <<IFEOF
DEVICE=a2065.device
UNIT=0
CONFIGURE=STATIC
ADDRESS=$SELF
NETMASK=$MASK
STATE=up
IFEOF

. "$ROOT/tools/sana2-stage.sh"
if [ -z "${AMINETXDUO_SANA2_DRIVER:-}" ] && [ "$BOARD" != a2065 ]; then
    _want=$(sana2_driver_for "$BOARD")
    _have=$(sana2_local_driver "$_want")
    [ -n "$_have" ] && [ -f "$_have" ] &&
        export AMINETXDUO_SANA2_DRIVER="$_have"
fi
sana2_stage "$BOARD" "$STAGE/devs"

cp "$BSD" "$STAGE/libs/bsdsocket.library"
cp "$TLS" "$STAGE/libs/tls.library"
cp "$TOOLS/AddNetInterface" "$STAGE/AddNetInterface"
cp "$TOOLS/fetch"           "$STAGE/fetch"
cp "$LOOP"                  "$STAGE/TlsLoop"

# fetch reads the trust store from DEVS:Internet/certificates and nowhere else,
# so the root goes there under the name it looks for.
mkdir -p "$STAGE/devs/Internet"
cp "$PKI/leaf.cert.der" "$STAGE/devs/Internet/fetchtls.cert"
cp "$PKI/leaf.key.der"  "$STAGE/devs/Internet/fetchtls.key"
cp "$PKI/store"         "$STAGE/devs/Internet/certificates"

# The name the certificate is issued to, pointed at the machine itself: fetch
# takes the host out of the URL for the connect AND for the name check, so the
# two cannot be separated the way TlsLoop CLIENT separates them.
printf '127.0.0.1 %s\n' "$NAME" >> "$STAGE/devs/Internet/hosts"

# The `wait 10` between the two is not padding: ToolsSmoke's `&` returns as
# soon as the process is created, and the first run of this arm had fetch
# fail with errno 61 after 300 ms because TlsLoop had not reached bind()
# yet.  The server prints server_listen=ok well inside that window.
cat > "$STAGE/commands.txt" <<EOF
SYS:AddNetInterface eth0
&SYS:TlsLoop SERVER PORT $PORT CERT DEVS:Internet/fetchtls.cert KEY DEVS:Internet/fetchtls.key KEYTYPE EC HTTP WAIT 240 >DH0:server.txt
wait 10
SYS:fetch https://$NAME:$PORT/ TIMEOUT 180 TO DH0:body.txt >DH0:client.txt
wait 5
EOF

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-fetchtls}"

# 02:41:4d:46:<runbyte>:7a.  Fourth byte 0x46 is this harness's own, clear of
# amiberry-run.sh's 0x49, run-tlsloop.sh's 0x54, run-cardsweep6.sh's 0x4b and
# run-srcsel.sh's 0x4c.
RUNBYTE="${AMINETXDUO_FETCHTLS_RUNBYTE:-$(printf '%02x' $((RANDOM % 256)))}"
export AMINETXDUO_AMIBERRY_MAC="${AMINETXDUO_AMIBERRY_MAC:-02:41:4d:46:$RUNBYTE:7a}"

HD="$ROOT/build/amiberry-testhd-$AMINETXDUO_RUN_TAG"

echo "fetchtls_model=$MODEL"
echo "fetchtls_board=$BOARD"
echo "fetchtls_backend=$IFACE"
echo "fetchtls_mac=$AMINETXDUO_AMIBERRY_MAC"
echo "fetchtls_url=https://$NAME:$PORT/"

set +e
"$ROOT/tools/amiberry-run.sh" -N "$BOARD" -B "$IFACE" -m "$MODEL" \
    -t "$TIMEOUT" \
    "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs" \
    "$STAGE/AddNetInterface" "$STAGE/fetch" "$STAGE/TlsLoop"
RUN_RC=$?
set -e

echo "fetchtls_run_rc=$RUN_RC"

FAILED=0
fail() { echo "$1=FAIL"; FAILED=1; }
pass() { echo "$1=ok"; }

for f in server client body; do
    echo
    echo "===================== $f ====================="
    if [ -f "$HD/$f.txt" ]; then cat "$HD/$f.txt"; else echo "(no transcript)"; fi
    echo "=============================================="
done
echo

# ---- the server half -----------------------------------------------------

if [ -f "$HD/server.txt" ]; then
    checks=$(sed -n 's/^tlsloop_checks=//p'   "$HD/server.txt" | head -1)
    fails=$(sed -n  's/^tlsloop_failures=//p' "$HD/server.txt" | head -1)
    echo "fetchtls_server_checks=${checks:-none}"
    echo "fetchtls_server_failures=${fails:-none}"
    if [ -n "${fails:-}" ] && [ "${checks:-0}" != 0 ] && [ "$fails" -eq 0 ]
    then pass fetchtls_server_ran
    else fail fetchtls_server_ran
    fi
    for key in server_handshake server_read_matches server_write; do
        if grep -Fqx "$key=ok" "$HD/server.txt"
        then pass "fetchtls_$key"
        else fail "fetchtls_$key"
        fi
    done
    ver=$(sed -n 's/^server_version=//p' "$HD/server.txt" | head -1)
    echo "fetchtls_server_version=${ver:-none}"
else
    fail fetchtls_server_transcript
fi

# ---- the client half, which is the point ---------------------------------
#
# fetch's own words, because a gate on the shipped command has to fail when the
# shipped command's report changes and not only when the bytes do.

if [ -f "$HD/client.txt" ]; then
    if grep -q "HTTP/1.1 200 OK" "$HD/client.txt"
    then pass fetchtls_client_status
    else fail fetchtls_client_status
    fi
    if grep -q "chain verified" "$HD/client.txt"
    then pass fetchtls_client_verified
    else fail fetchtls_client_verified
    fi
    if grep -qi "without TLS support" "$HD/client.txt"; then
        echo "fetchtls_client_nostack=1"
        fail fetchtls_client_reached_tls
    else
        pass fetchtls_client_reached_tls
    fi
else
    fail fetchtls_client_transcript
fi

# ---- the bytes -----------------------------------------------------------
#
# The body and not the byte count: a handshake that completes and then moves
# the wrong plaintext is the failure this arm exists to see.

WANT='AmiNetXDuo fetch over TLS'
if [ -f "$HD/body.txt" ]; then
    got=$(wc -c < "$HD/body.txt" | tr -d ' ')
    echo "fetchtls_body_bytes=$got"
    if [ "$got" = 26 ]; then pass fetchtls_body_length
    else fail fetchtls_body_length; fi
    if [ "$(head -c 25 "$HD/body.txt")" = "$WANT" ]
    then pass fetchtls_body_matches
    else fail fetchtls_body_matches
    fi
else
    echo "fetchtls_body_bytes=0"
    fail fetchtls_body_present
fi

echo
if [ "$FAILED" -ne 0 ]; then
    echo "fetchtls=FAILED"
    exit 1
fi

echo "fetchtls=PASSED"
exit 0
