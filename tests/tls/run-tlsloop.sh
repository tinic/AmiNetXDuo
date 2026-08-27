#!/usr/bin/env bash
# tls.library AS THE SERVER, against tls.library as the client, over 127.0.0.1
# inside one guest.  No peer, no bridge traffic: both ends are Shell processes
# on the Amiga and the bytes never leave it.
#
# Two rounds in one boot, because the two key types take different paths
# through nx_secure's server half:
#   round ec   -- an EC (SEC1 DER) key, ECDSA CertificateVerify
#   round rsa  -- an RSA (PKCS#1 DER) key
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

MODEL=A1200
TIMEOUT=900
BUILD="${AMINETXDUO_BUILD:-build/cm}"
BOARD="${AMINETXDUO_AMIBERRY_BOARD:-a2065}"
IFACE="${AMINETXDUO_AMIBERRY_BACKEND:-ens18}"
EC_PORT=7443
RSA_PORT=7444

while getopts "m:t:b:N:B:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        N) BOARD="$OPTARG" ;;
        B) IFACE="$OPTARG" ;;
        *) echo "usage: $0 [-m model] [-t seconds] [-b builddir] [-N board] [-B backend]" >&2; exit 2 ;;
    esac
done

case "$IFACE" in
    slirp|slirp_inbound|none)
        echo "tlsloop_backend=refused:$IFACE" >&2
        echo "This harness is bridged only.  -B names a host interface." >&2
        exit 2
        ;;
esac

TOOLS="$ROOT/$BUILD/src/tools"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"
TLS="$ROOT/$BUILD/src/tlslib/tls.library"
LOOP="$ROOT/$BUILD/tests/tls/TlsLoop"

for f in "$TOOLS/ToolsSmoke" "$TOOLS/AddNetInterface" "$LOOP" "$BSD" "$TLS"; do
    [ -f "$f" ] || { echo "tlsloop_stage=missing:$f" >&2; exit 2; }
done

command -v openssl > /dev/null || { echo "tlsloop_stage=missing:openssl" >&2; exit 2; }

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
    echo "tlsloop_stage=missing:a2065.device" >&2
    echo "Set AMINETXDUO_A2065=<path>." >&2
    exit 2
}

# ------------------------------------------------------------------ PKI ---
#
# A root and TWO leaves signed DIRECTLY by it, one EC and one RSA.  Directly,
# because src/tlslib/tls_server.c presents ONE certificate and not a chain, so
# an intermediate would leave the client with a gap it cannot fill.  The
# validity window is deliberately wide: an Amiga with no battery clock reads a
# date tls.library treats as "unknown", and one with a battery clock reads the
# host's, and the certificate has to be valid under both.

PKI="$ROOT/build/tlsloop-pki"
rm -rf "$PKI"
mkdir -p "$PKI"

cat > "$PKI/ca.cnf" <<'EOF'
basicConstraints=critical,CA:TRUE
keyUsage=critical,keyCertSign,cRLSign
subjectKeyIdentifier=hash
EOF

cat > "$PKI/leaf.cnf" <<'EOF'
basicConstraints=critical,CA:FALSE
keyUsage=critical,digitalSignature,keyEncipherment
extendedKeyUsage=serverAuth
subjectAltName=DNS:tlsloop.test
subjectKeyIdentifier=hash
EOF

NOT_BEFORE=20200101000000Z
NOT_AFTER=20450101000000Z

openssl genrsa -out "$PKI/root.key.pem" 2048 2>/dev/null
openssl req -new -x509 -key "$PKI/root.key.pem" -sha256 \
    -not_before "$NOT_BEFORE" -not_after "$NOT_AFTER" \
    -subj "/CN=TlsLoop Root" -extensions v3 -config <(
        printf '[req]\ndistinguished_name=dn\n[dn]\n[v3]\n'
        cat "$PKI/ca.cnf") \
    -out "$PKI/root.cert.pem" 2>/dev/null

mkleaf() {   # mkleaf <name> <rsa|ec>
    local name="$1" kind="$2"

    if [ "$kind" = ec ]; then
        openssl ecparam -name prime256v1 -genkey -noout \
            -out "$PKI/$name.key.pem" 2>/dev/null
        openssl ec -in "$PKI/$name.key.pem" -outform DER \
            -out "$PKI/$name.key.der" 2>/dev/null
    else
        openssl genrsa -out "$PKI/$name.key.pem" 2048 2>/dev/null
        openssl rsa -in "$PKI/$name.key.pem" -outform DER -traditional \
            -out "$PKI/$name.key.der" 2>/dev/null
    fi

    openssl req -new -key "$PKI/$name.key.pem" -subj "/CN=tlsloop.test" \
        -out "$PKI/$name.csr" 2>/dev/null
    openssl x509 -req -in "$PKI/$name.csr" -sha256 \
        -CA "$PKI/root.cert.pem" -CAkey "$PKI/root.key.pem" \
        -CAcreateserial -not_before "$NOT_BEFORE" -not_after "$NOT_AFTER" \
        -extfile "$PKI/leaf.cnf" -out "$PKI/$name.cert.pem" 2>/dev/null
    openssl x509 -in "$PKI/$name.cert.pem" -outform DER \
        -out "$PKI/$name.cert.der" 2>/dev/null
}

mkleaf leafec  ec
mkleaf leafrsa rsa

python3 "$ROOT/tools/mkcertstore.py" --output "$PKI/store" \
    "$PKI/root.cert.pem" > /dev/null

for f in leafec.cert.der leafec.key.der leafrsa.cert.der leafrsa.key.der store; do
    [ -s "$PKI/$f" ] || { echo "tlsloop_pki=missing:$f" >&2; exit 2; }
done

echo "tlsloop_pki_ec_cert=$(wc -c < "$PKI/leafec.cert.der" | tr -d ' ')"
echo "tlsloop_pki_ec_key=$(wc -c < "$PKI/leafec.key.der" | tr -d ' ')"
echo "tlsloop_pki_rsa_cert=$(wc -c < "$PKI/leafrsa.cert.der" | tr -d ' ')"
echo "tlsloop_pki_rsa_key=$(wc -c < "$PKI/leafrsa.key.der" | tr -d ' ')"
echo "tlsloop_pki_store=$(wc -c < "$PKI/store" | tr -d ' ')"

# ---------------------------------------------------------------- stage ---

SELF="${AMINETXDUO_TLSLOOP_SELF:-10.79.0.2}"
MASK="${AMINETXDUO_TLSLOOP_MASK:-255.255.255.0}"

STAGE="$ROOT/build/tlsloop-stage"
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
cp "$LOOP" "$STAGE/TlsLoop"

mkdir -p "$STAGE/devs/Internet"
cp "$PKI/leafec.cert.der"  "$STAGE/devs/Internet/ec.cert"
cp "$PKI/leafec.key.der"   "$STAGE/devs/Internet/ec.key"
cp "$PKI/leafrsa.cert.der" "$STAGE/devs/Internet/rsa.cert"
cp "$PKI/leafrsa.key.der"  "$STAGE/devs/Internet/rsa.key"
cp "$PKI/store"            "$STAGE/devs/Internet/tlsloopstore"

cat > "$STAGE/commands.txt" <<EOF
SYS:AddNetInterface eth0
&SYS:TlsLoop SERVER PORT $EC_PORT CERT DEVS:Internet/ec.cert KEY DEVS:Internet/ec.key KEYTYPE EC WAIT 240 >DH0:ec-server.txt
SYS:TlsLoop CLIENT PORT $EC_PORT HOST tlsloop.test STORE DEVS:Internet/tlsloopstore WAIT 60 >DH0:ec-client.txt
&SYS:TlsLoop SERVER PORT $RSA_PORT CERT DEVS:Internet/rsa.cert KEY DEVS:Internet/rsa.key KEYTYPE RSA WAIT 240 >DH0:rsa-server.txt
SYS:TlsLoop CLIENT PORT $RSA_PORT HOST tlsloop.test STORE DEVS:Internet/tlsloopstore WAIT 60 >DH0:rsa-client.txt
wait 10
EOF

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-tlsloop}"

# 02:41:4d:54:<runbyte>:7a.  Fourth byte 0x54 is this harness's own, clear of
# amiberry-run.sh's 0x49, run-cardsweep6.sh's 0x4b and run-srcsel.sh's 0x4c.
RUNBYTE="${AMINETXDUO_TLSLOOP_RUNBYTE:-$(printf '%02x' $((RANDOM % 256)))}"
export AMINETXDUO_AMIBERRY_MAC="${AMINETXDUO_AMIBERRY_MAC:-02:41:4d:54:$RUNBYTE:7a}"

HD="$ROOT/build/amiberry-testhd-$AMINETXDUO_RUN_TAG"

echo "tlsloop_model=$MODEL"
echo "tlsloop_board=$BOARD"
echo "tlsloop_backend=$IFACE"
echo "tlsloop_mac=$AMINETXDUO_AMIBERRY_MAC"

set +e
"$ROOT/tools/amiberry-run.sh" -N "$BOARD" -B "$IFACE" -m "$MODEL" \
    -t "$TIMEOUT" \
    "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs" \
    "$STAGE/AddNetInterface" "$STAGE/TlsLoop"
RUN_RC=$?
set -e

echo "tlsloop_run_rc=$RUN_RC"

FAILED=0
fail() { echo "$1=FAIL"; FAILED=1; }
pass() { echo "$1=ok"; }

for round in ec rsa; do
    for side in server client; do
        f="$HD/$round-$side.txt"
        echo
        echo "===================== $round $side ====================="
        if [ -f "$f" ]; then
            cat "$f"
        else
            echo "(no transcript)"
        fi
        echo "======================================================="

        if [ ! -f "$f" ]; then
            fail "tlsloop_${round}_${side}_transcript"
            continue
        fi

        checks=$(sed -n 's/^tlsloop_checks=//p' "$f" | head -1)
        fails=$(sed -n 's/^tlsloop_failures=//p' "$f" | head -1)
        echo "tlsloop_${round}_${side}_checks=${checks:-none}"
        echo "tlsloop_${round}_${side}_failures=${fails:-none}"

        if [ -z "${fails:-}" ] || [ "${checks:-0}" = 0 ]; then
            fail "tlsloop_${round}_${side}_ran"
        elif [ "$fails" -eq 0 ]; then
            pass "tlsloop_${round}_${side}_ran"
        else
            fail "tlsloop_${round}_${side}_ran"
        fi
    done

    for want in \
        "server:server_handshake" \
        "server:server_read_matches" \
        "server:server_write" \
        "client:client_handshake" \
        "client:client_read_matches" \
        "client:client_write"
    do
        side="${want%%:*}"
        key="${want##*:}"
        if grep -Fqx "$key=ok" "$HD/$round-$side.txt" 2>/dev/null; then
            pass "tlsloop_${round}_${key}"
        else
            fail "tlsloop_${round}_${key}"
        fi
    done

    ver=$(sed -n 's/^client_version=//p' "$HD/$round-client.txt" 2>/dev/null | head -1)
    suite=$(sed -n 's/^client_suite=//p' "$HD/$round-client.txt" 2>/dev/null | head -1)
    ms=$(sed -n 's/^server_handshake_ms=//p' "$HD/$round-server.txt" 2>/dev/null | head -1)
    verified=$(sed -n 's/^client_verified=//p' "$HD/$round-client.txt" 2>/dev/null | head -1)
    echo "tlsloop_${round}_version=${ver:-none}"
    echo "tlsloop_${round}_suite=${suite:-none}"
    echo "tlsloop_${round}_server_handshake_ms=${ms:-none}"
    echo "tlsloop_${round}_client_verified=${verified:-none}"

    if [ "${verified:-0}" = 1 ]; then
        pass "tlsloop_${round}_chain_verified"
    else
        fail "tlsloop_${round}_chain_verified"
    fi
done

echo
if [ "$FAILED" -ne 0 ]; then
    echo "tlsloop=FAILED"
    exit 1
fi

echo "tlsloop=PASSED"
exit 0
