#!/usr/bin/env bash
# bebboget against this stack, beside our own fetch, under FS-UAE.
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
MODEL=A1200
TIMEOUT=1200
CLOCK=""
BUILD="${AMINETXDUO_BUILD:-build/cm}"
PERF=0
ENFORCE=0
COMMANDS=""
PINNED=0
PORT="${AMINETXDUO_BEBBOGET_PORT:-8443}"

while getopts "m:t:b:k:xE1C:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        k) CLOCK="$OPTARG" ;;
        x) PERF=1 ;;
        E) ENFORCE=1 ;;
        1) PINNED=1 ;;
        C) COMMANDS="$OPTARG" ;;
        *) echo "usage: $0 [-m model] [-t secs] [-b builddir] [-k MHz] [-x] [-E] [-1] [-C file]" >&2; exit 2 ;;
    esac
done

BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"
TLS="$ROOT/$BUILD/src/tlslib/tls.library"
ADDIF="$ROOT/$BUILD/src/tools/AddNetInterface"
FETCH="$ROOT/$BUILD/src/tools/fetch"
for f in "$BSD" "$ADDIF"; do
    [ -f "$f" ] || { echo "missing $f, build bsdsocket_library AddNetInterface" >&2; exit 2; }
done
HAVE_FETCH=1
for f in "$TLS" "$FETCH"; do
    [ -f "$f" ] || HAVE_FETCH=0
done
[ "$HAVE_FETCH" = "1" ] || echo "!! no fetch/tls.library in $BUILD, bebboget arm only, no comparison" >&2

command -v python3 >/dev/null 2>&1 || { echo "python3 is needed for the server" >&2; exit 2; }
command -v openssl >/dev/null 2>&1 || { echo "openssl is needed for the payloads and cert" >&2; exit 2; }


BEB="${AMINETXDUO_BEBBOGET_DIR:-$HOME/amiga-assets/bebboget}"
[ -d "$BEB" ] || {
    echo "no bebboget binaries in $BEB, set AMINETXDUO_BEBBOGET_DIR" >&2
    exit 2
}
BEBVER=$(cat "$BEB/VERSION" 2>/dev/null || echo unknown)


LOCALE="${AMINETXDUO_LOCALE_LIBRARY:-}"
if [ -z "$LOCALE" ]; then
    for candidate in \
        "$ROOT/build/wb31-sys/Libs/locale.library" \
        "$HOME/AmiNetXDuo/build/wb31-sys/Libs/locale.library" \
        "$HOME/amigaos/workbench/libs/locale.library" \
        "$ROOT/build/locale.library"
    do
        [ -f "$candidate" ] && { LOCALE="$candidate"; break; }
    done
fi
[ -n "$LOCALE" ] && [ -f "$LOCALE" ] || {
    echo "No locale.library found, and bebboget hangs without one." >&2
    echo "  Set AMINETXDUO_LOCALE_LIBRARY=<path>, or unpack a Workbench 3.1 set" >&2
    echo "  with install/test/run-workbench.sh, which writes build/wb31-sys." >&2
    exit 2
}


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


WWW="$ROOT/build/bebboget-test/www"
CERTDIR="$ROOT/build/bebboget-test"
mkdir -p "$WWW"

mkpayload() {
    local path="$1" size="$2"
    [ -f "$path" ] && [ "$(wc -c < "$path" | tr -d ' ')" = "$size" ] && return 0
    head -c "$size" /dev/zero \
        | openssl enc -aes-128-ctr -nosalt -k aminetxduo-payload > "$path"
}
mkpayload "$WWW/tiny.bin"   45
mkpayload "$WWW/mid.bin"    65536
mkpayload "$WWW/big.bin"    262144

if [ ! -f "$CERTDIR/server.crt" ]; then
    echo "==> self-signed server certificate"
    openssl req -x509 -newkey rsa:2048 -nodes -days 3650 \
        -keyout "$CERTDIR/server.key" -out "$CERTDIR/server.crt" \
        -subj "/CN=10.0.2.2" -addext "subjectAltName=IP:10.0.2.2" \
        >/dev/null 2>&1
fi

SRVLOG="$ROOT/build/bebboget-test/httpsd.log"
SRVARGS=()
if [ "$PINNED" = "1" ]; then
    # 0xC027, the whole intersection of the two clients' TLS 1.2 offers.
    SRVARGS+=(--tls12 --suite ECDHE-RSA-AES128-SHA256)
fi
python3 "$ROOT/tests/bebboget/httpsd.py" \
    --dir "$WWW" --cert "$CERTDIR/server.crt" --key "$CERTDIR/server.key" \
    --port "$PORT" --bind 0.0.0.0 ${SRVARGS[@]+"${SRVARGS[@]}"} > "$SRVLOG" 2>&1 &
SERVER_PID=$!

cleanup() {
    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
}
trap cleanup EXIT INT TERM HUP

for _ in 1 2 3 4 5 6 7 8 9 10; do
    grep -q '^listening' "$SRVLOG" 2>/dev/null && break
    sleep 0.5
done
grep -q '^listening' "$SRVLOG" || {
    echo "the HTTPS server did not start:" >&2; cat "$SRVLOG" >&2; exit 2; }
echo "==> HTTPS server on 0.0.0.0:$PORT serving $WWW"


TAG="${AMINETXDUO_RUN_TAG:-bebboget}"
STAGE="$ROOT/build/bebboget-stage-$TAG"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$A2065"  "$STAGE/devs/a2065.device"
cp "$BSD"    "$STAGE/libs/bsdsocket.library"
cp "$LOCALE" "$STAGE/libs/locale.library"
cp "$ADDIF"  "$STAGE/AddNetInterface"
cp "$BEB/bebboget" "$STAGE/bebboget"
if [ "$HAVE_FETCH" = "1" ]; then
    cp "$TLS"   "$STAGE/libs/tls.library"
    cp "$FETCH" "$STAGE/fetch"
fi

BASEDIR="$ROOT/build/fsuae-base-$TAG"
mkdir -p "$BASEDIR/Configurations"
cat > "$BASEDIR/Configurations/Host.fs-uae" <<'EOF'
[fs-uae]
floppy_drive_volume = 0
floppy_drive_volume_empty = 0
bsdsocket_library = 0
EOF

HOST="${AMINETXDUO_HTTPS_HOST:-10.0.2.2}"
URL="https://$HOST:$PORT"


if [ -n "$COMMANDS" ]; then
    cp "$COMMANDS" "$STAGE/commands.txt"
    echo "==> command list: $COMMANDS"
else
    BGOPT="-q -O --sloppy"
    [ "$PINNED" = "1" ] && BGOPT="$BGOPT --max 3 --cipher TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA256"

    : > "$STAGE/commands.txt"
    echo "SYS:AddNetInterface eth0" >> "$STAGE/commands.txt"
    echo "SYS:bebboget $BGOPT -o DH0:warm-bg.bin $URL/tiny.bin" >> "$STAGE/commands.txt"
    for sz in tiny mid big; do
        echo "SYS:bebboget $BGOPT -o DH0:bg-$sz.bin $URL/$sz.bin" >> "$STAGE/commands.txt"
    done
    if [ "$HAVE_FETCH" = "1" ]; then
        echo "SYS:fetch $URL/tiny.bin TO DH0:warm-ft.bin QUIET NOVERIFY" >> "$STAGE/commands.txt"
        for sz in tiny mid big; do
            echo "SYS:fetch $URL/$sz.bin TO DH0:ft-$sz.bin QUIET NOVERIFY" >> "$STAGE/commands.txt"
        done
    fi
fi


. "$ROOT/tools/amiga-toolchain.sh"

RUNNER="$ROOT/build/clients/ClientRun"
mkdir -p "$ROOT/build/clients"
if [ ! -x "$RUNNER" ] || [ "$ROOT/clients/dropbear/clientrun.c" -nt "$RUNNER" ]; then
    echo "==> building ClientRun"
    case "${CPU:-}${MODEL:-}" in
        *68000*|*A500*|*A600*|*A2000*) _guest_arch="-m68000" ;;
        *)                             _guest_arch="-m68020" ;;
    esac
    "$AMIGA_GCC" -O2 "$_guest_arch" -fomit-frame-pointer -Wall -I"$AMIGA_NDK" \
                 -o "$RUNNER" "$ROOT/clients/dropbear/clientrun.c"
fi

echo "==> bebboget $BEBVER from $BEB"
echo "==> target: $URL"

export AMINETXDUO_RUN_TAG="$TAG"

CPUARG=()
[ -z "$CLOCK" ] || CPUARG+=(-k "$CLOCK")
[ "$PERF" = "1" ] && CPUARG+=(-x)

STAGED=("$STAGE/devs" "$STAGE/libs" "$STAGE/bebboget" "$STAGE/AddNetInterface"
        "$STAGE/commands.txt")
[ "$HAVE_FETCH" = "1" ] && STAGED+=("$STAGE/fetch")

set +e
if [ "$ENFORCE" = "1" ]; then
    "$ROOT/tools/enforcer-run.sh" -n -m -t "$TIMEOUT" -T "$TAG" \
        "$RUNNER" "${STAGED[@]}"
else
    "$ROOT/tools/amiberry-run.sh" -N a2065 -m "$MODEL" -t "$TIMEOUT" ${CPUARG[@]+"${CPUARG[@]}"} \
        "$RUNNER" "${STAGED[@]}"
fi
RC=$?
set -e

HD="$ROOT/build/amiberry-testhd-$TAG"
echo ""
echo "==> DH0:client.txt"
cat "$HD/client.txt" 2>/dev/null || echo "(none)"
echo ""

echo "==> what the server negotiated, per request"
grep -E "GET" "$SRVLOG" | awk '{print "    " $2, $3, $5, $6}' | sort | uniq -c
echo ""

set +e
"$ROOT/tests/bebboget/check.sh" "$HD" "$WWW"
VERDICT=$?
set -e

echo "fs-uae exit status was $RC; the verdict line above is the result."
exit "$VERDICT"
