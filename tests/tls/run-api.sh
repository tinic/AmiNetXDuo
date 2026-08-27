#!/usr/bin/env bash
# Fetch a real HTTPS URL through tls.library, under Amiberry on a bridged card.
# The a2065.device driver is not ours to ship: point AMINETXDUO_A2065 at one,
# or drop a copy in build/a2065.device.  Every other board's driver comes out
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
MODEL=A1200
TIMEOUT=300
CPU=""
BUILD="${AMINETXDUO_BUILD:-build/tls}"
BOARD="${AMINETXDUO_AMIBERRY_BOARD:-a2065}"
IFACE="${AMINETXDUO_AMIBERRY_BACKEND:-ens18}"

while getopts "m:t:c:b:N:B:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        c) CPU="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        N) BOARD="$OPTARG" ;;
        B) IFACE="$OPTARG" ;;
        *) echo "usage: $0 [-m model] [-t seconds] [-c cpu] [-b builddir] [-N board] [-B backend]" >&2; exit 2 ;;
    esac
done

case "$IFACE" in
    slirp|slirp_inbound|none)
        echo "tls-api_backend=refused:$IFACE" >&2
        echo "This harness fetches a real HTTPS URL and is bridged only." >&2
        echo "-B names a host interface." >&2
        exit 2
        ;;
esac

EXE="$ROOT/$BUILD/tests/tls/tls_api"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"
TLS="$ROOT/$BUILD/src/tlslib/tls.library"

for f in "$EXE" "$BSD" "$TLS"; do
    [ -f "$f" ] || {
        echo "missing $f, build tls_api bsdsocket_library tls_library first" >&2
        exit 2
    }
done

STORE="$ROOT/$BUILD/certificates"
if [ ! -f "$STORE" ]; then
    BUNDLE="${AMINETXDUO_CA_BUNDLE:-}"
    if [ -z "$BUNDLE" ]; then
        for candidate in \
            "$ROOT/dist/cacert.pem" \
            "/etc/ssl/cert.pem" \
            "/etc/ssl/certs/ca-certificates.crt" \
            "/etc/pki/tls/certs/ca-bundle.crt"
        do
            [ -f "$candidate" ] && { BUNDLE="$candidate"; break; }
        done
    fi
    [ -n "$BUNDLE" ] && [ -f "$BUNDLE" ] || {
        cat >&2 <<'EOF'
No CA bundle found, so there is no trust store to verify against.

  curl -o dist/cacert.pem https://curl.se/ca/cacert.pem

or set AMINETXDUO_CA_BUNDLE=<path to a PEM bundle>.
EOF
        exit 2
    }
    python3 "$ROOT/tools/mkcertstore.py" --output "$STORE" "$BUNDLE"
fi

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

STAGE="$ROOT/build/tls-api-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$A2065" "$STAGE/devs/a2065.device"
cp "$BSD" "$STAGE/libs/bsdsocket.library"
cp "$TLS" "$STAGE/libs/tls.library"
cp "$STORE" "$STAGE/devs/Internet/certificates"

# The staged file already carries no nameserver line, for the reason written
# in it; this is here so that the phase below still reads a resolver file it
# wrote itself if that ever changes.
cat > "$STAGE/devs/Internet/name_resolution" <<'NREOF'
# No nameserver line: the DHCP lease on the bridge carries one, and a second
# server this file cannot reach costs the resolver's failover time on every
# lookup before it gets used.
domain localdomain
NREOF

. "$ROOT/tools/sana2-stage.sh"
if [ -z "${AMINETXDUO_SANA2_DRIVER:-}" ] && [ "$BOARD" != a2065 ]; then
    _want=$(sana2_driver_for "$BOARD")
    _have=$(sana2_local_driver "$_want")
    [ -n "$_have" ] && [ -f "$_have" ] &&
        export AMINETXDUO_SANA2_DRIVER="$_have"
fi
sana2_stage "$BOARD" "$STAGE/devs"

echo "==> trust store: $(wc -c < "$STORE" | tr -d ' ') bytes"

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-tlsapi}"

. "$ROOT/tools/test-verdict.sh"

verdict() {
    verdict_guest "tls-api" 26 "$1" \
        "$(verdict_hd_amiberry)/stdout.txt" \
        "$(verdict_serial_amiberry)" && exit 0
    exit $?
}

CPUARG=()
[ -z "$CPU" ] || CPUARG=(-c "$CPU")

set +e
echo "==> booting $MODEL under Amiberry, $BOARD on $IFACE"
"$ROOT/tools/amiberry-run.sh" -N "$BOARD" -B "$IFACE" -m "$MODEL" \
     -t "$TIMEOUT" "${CPUARG[@]}" \
     "$EXE" "$STAGE/devs" "$STAGE/libs"
RUN_RC=$?
set -e
verdict "$RUN_RC"
