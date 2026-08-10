#!/usr/bin/env bash
#
# Fetch a real HTTPS URL through tls.library, under FS-UAE on SLIRP.
#
#   tests/tls/run-api.sh [-m MODEL] [-t SECONDS] [-c CPU] [-b BUILDDIR]
#
# Unlike tests/tls/run-https.sh this stages the two SHARED LIBRARIES and the
# trust store, because the program under test is linked against none of our
# code, it opens LIBS:bsdsocket.library and LIBS:tls.library by name and
# verifies the chain against DEVS:Internet/certificates.
#
# The trust store is built here rather than checked in: a CA bundle changes
# every few weeks and a stale copy in git would be worse than none.  Set
# AMINETXDUO_CA_BUNDLE, or drop a cacert.pem in dist/, or let it find the
# host's.
#
# The a2065.device driver is not ours to ship: point AMINETXDUO_A2065 at one,
# or drop a copy in build/a2065.device.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
MODEL=A1200
TIMEOUT=300
CPU=""
BUILD="${AMINETXDUO_BUILD:-build/tls}"

while getopts "m:t:c:b:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        c) CPU="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        *) echo "usage: $0 [-m model] [-t seconds] [-c cpu] [-b builddir]" >&2; exit 2 ;;
    esac
done

EXE="$ROOT/$BUILD/tests/tls/tls_api"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"
TLS="$ROOT/$BUILD/src/tlslib/tls.library"

for f in "$EXE" "$BSD" "$TLS"; do
    [ -f "$f" ] || {
        echo "missing $f, build tls_api bsdsocket_library tls_library first" >&2
        exit 2
    }
done

# ---------------------------------------------------------- trust store --

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

# ------------------------------------------------------------- a2065 -----

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

# ------------------------------------------------------------- staging ---

STAGE="$ROOT/build/tls-api-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$A2065" "$STAGE/devs/a2065.device"
cp "$BSD" "$STAGE/libs/bsdsocket.library"
cp "$TLS" "$STAGE/libs/tls.library"
cp "$STORE" "$STAGE/devs/Internet/certificates"

echo "==> trust store: $(wc -c < "$STORE" | tr -d ' ') bytes"

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-tlsapi}"

# ---------------------------------------------------------- the verdict ---
#
# This used to end in `exec <runner>`, so the script's exit status was the
# guest's own return code: a guest that opened nothing, ran no checks and
# returned 0 was a pass, and so was one whose transcript never arrived.
# tools/test-verdict.sh reads the guest's own counters instead, puts a floor
# under the number of checks, and fails loudly and by name when there is no
# transcript at all.
. "$ROOT/tools/test-verdict.sh"

verdict() {
    # 0 pass, 1 fail, 77 the guest skipped: all three are carried out.
    verdict_guest "tls-api" 1 "$1" \
        "$(verdict_hd_amiberry)/stdout.txt" \
        "$(verdict_serial_amiberry)" && exit 0
    exit $?
}

CPUARG=()
[ -z "$CPU" ] || CPUARG=(-c "$CPU")

set +e
"$ROOT/tools/amiberry-run.sh" -N a2065 -m "$MODEL" -t "$TIMEOUT" "${CPUARG[@]}" \
     "$EXE" "$STAGE/devs" "$STAGE/libs"
RUN_RC=$?
set -e
verdict "$RUN_RC"
