#!/usr/bin/env bash
# Put `httpd` on the LAN under Amiberry, so that real WebDAV clients can be
# pointed at it.
# The a2065.device driver is not ours to ship: AMINETXDUO_A2065=<path>, or
# drop a copy in build/a2065.device.
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

WINDOW=300
ADDRESS=192.168.1.240
NETMASK=255.255.255.0
GATEWAY=192.168.1.1
PORT=8080
MODEL=A1200
BACKEND=ens18
BUILD="${AMINETXDUO_BUILD:-build/cm}"

BOARD="${AMINETXDUO_HTTPD_BOARD:-a2065}"
IFDEVICE="${AMINETXDUO_IFDEVICE:-a2065.device}"

while getopts "t:a:p:b:B:m:g:N:" opt; do
    case "$opt" in
        N) BOARD="$OPTARG" ;;
        t) WINDOW="$OPTARG" ;;
        a) ADDRESS="$OPTARG" ;;
        p) PORT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        B) BACKEND="$OPTARG" ;;
        m) MODEL="$OPTARG" ;;
        g) GATEWAY="$OPTARG" ;;
        *) echo "usage: $0 [-t seconds] [-a address] [-p port] [-b builddir] [-B backend] [-m model]" >&2; exit 2 ;;
    esac
done

TOOLS="$ROOT/$BUILD/src/tools"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"

for f in "$TOOLS/httpd" "$BSD"; do
    [ -f "$f" ] || { echo "missing $f, build the tree first" >&2; exit 2; }
done

A2065="${AMINETXDUO_A2065:-}"
if [ -z "$A2065" ]; then
    for candidate in "$ROOT/build/a2065.device" "$HOME/amiga-assets/devs/a2065.device"; do
        [ -f "$candidate" ] && { A2065="$candidate"; break; }
    done
fi
[ -n "$A2065" ] && [ -f "$A2065" ] || {
    echo "No a2065.device found.  Set AMINETXDUO_A2065=<path>." >&2
    exit 2
}

STAGE="$ROOT/build/httpd-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs" "$STAGE/Public/Docs" "$STAGE/Public/Empty Drawer"

cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$A2065" "$STAGE/devs/Networks/a2065.device" 2>/dev/null || {
    mkdir -p "$STAGE/devs/Networks"
    cp "$A2065" "$STAGE/devs/Networks/a2065.device"
}
cp "$BSD" "$STAGE/libs/bsdsocket.library"

[ -z "${AMINETXDUO_EXTRA_DRIVER:-}" ] || {
    cp "$AMINETXDUO_EXTRA_DRIVER" "$STAGE/devs/Networks/$(basename "$AMINETXDUO_EXTRA_DRIVER")"
    cp "$AMINETXDUO_EXTRA_DRIVER" "$STAGE/devs/$(basename "$AMINETXDUO_EXTRA_DRIVER")"
}

cat > "$STAGE/devs/NetInterfaces/eth0" <<EOF
DEVICE=$IFDEVICE
UNIT=0
CONFIGURE=STATIC
ADDRESS=$ADDRESS
NETMASK=$NETMASK
GATEWAY=$GATEWAY
EOF

echo "Hello from an Amiga." > "$STAGE/Public/readme.txt"
echo "<html><body><h1>Amiga</h1></body></html>" > "$STAGE/Public/index.html"
echo "one two three" > "$STAGE/Public/My File [!].txt"
echo "in a drawer" > "$STAGE/Public/Docs/notes.txt"
: > "$STAGE/Public/empty.dat"
dd if=/dev/urandom of="$STAGE/Public/blob.bin" bs=1024 count=512 status=none

echo "==> serving DH0:Public on http://$ADDRESS:$PORT/ for ${WINDOW}s"

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-httpd}"
HD="$ROOT/build/amiberry-testhd-$AMINETXDUO_RUN_TAG"

set +e
"$ROOT/tools/amiberry-run.sh" -N "$BOARD" -B "$BACKEND" -m "$MODEL" -t "$WINDOW" \
    -a "DH0:Public $PORT TRACE" \
    "$TOOLS/httpd" "$STAGE/devs" "$STAGE/libs" "$STAGE/Public" &
RUNNER=$!
set -e

ANSWERED=no
for _ in $(seq 1 "$((WINDOW / 2))"); do
    sleep 2
    kill -0 "$RUNNER" 2>/dev/null || break
    if curl -s -m 4 -o /dev/null -w '%{http_code}' \
            "http://$ADDRESS:$PORT/" 2>/dev/null | grep -q '^200$'; then
        ANSWERED=yes
        break
    fi
done

if [ "$ANSWERED" = yes ]; then
    echo "==> the guest answered a GET from this host"
    echo "==> point a client at http://$ADDRESS:$PORT/ now"
else
    echo "!! the guest never answered from this host" >&2
fi

wait "$RUNNER" 2>/dev/null || true

echo
echo "===================== what the clients sent ====================="
if [ -f "$HD/stdout.txt" ]; then
    cat "$HD/stdout.txt"
else
    echo "(the guest wrote no stdout.txt)"
fi
echo "================================================================="

[ "$ANSWERED" = yes ] || exit 1
