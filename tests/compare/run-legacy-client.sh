#!/usr/bin/env bash
#
# Run an unmodified third-party Amiga client against our bsdsocket.library.
#
#   tests/compare/run-legacy-client.sh -b BUILD [options]
#
#   -b DIR   build directory the library comes from (default build/cm)
#   -A FILE  .lha holding the client (default the curl below)
#   -x FILE  use this client binary instead of extracting one
#   -a DIR   extracted AmiSSL-v5-OS3 AmiSSL/ directory
#   -P PORT  base port for tests/peer/httppeer.py (default 7600)
#   -B N     bulk transfer size in bytes (default 307200)
#   -T TAG   run tag; results land in build/testhd-<tag>/
#   -t SECS  timeout
#   -m MODEL emulator profile (default A1200, the only timing profile)
#   -E       run under Enforcer instead of plain FS-UAE (68030, no timings)
#   -I       also run the one case that needs the internet and DNS
#   -W       run under WinUAE instead of FS-UAE
#   -N BOARD network card, WinUAE only (see tools/winuae-run.sh)
#
# -W is what measures a card other than the A2065.  FS-UAE emulates one NIC;
# WinUAE emulates nine, so a throughput number per card comes from there.  The
# peer then has to be somewhere the guest can reach: under FS-UAE it is the
# host at 10.0.2.2, under WinUAE the emulator is on another machine and the
# peer is on this one, so AMINETXDUO_PEER_IP has to name this machine's LAN
# address and the peer has to bind on all interfaces.
#
# The client is somebody else's binary, taken out of the archive it ships in.
# It is IPv4-only by construction, the packaged curl is configured
# --disable-ipv6, so it resolves through gethostbyname() and connects through
# struct sockaddr_in, which is the 1990s assumption an IPv6 library has to keep
# working with.
#
# Every transfer is checked against the bytes the peer meant to send.  The peer
# builds its bodies from one seeded buffer, so the host can hash what should
# have arrived without either side sending a manifest.
#
# Commands run under tests/compare/checkrunner.c, which gives each one 512 KB
# of stack.  A clib2 client with AmiSSL linked in does not survive a Shell
# default stack, and the failure looks like a hang rather than a crash.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)

BUILD="${AMINETXDUO_BUILD:-build/cm}"
ARCHIVE="${AMINETXDUO_LEGACY_LHA:-$ROOT/build/thirdparty-curl/curl-8.22.0-DEV-210726.lha}"
CLIENT=""
AMISSL="${AMINETXDUO_AMISSL:-$ROOT/build/thirdparty-curl/xamissl/AmiSSL}"
BASE_PORT=7600
BULK=307200
TAG=""
TIMEOUT=600
MODEL=A1200
ENFORCE=0
INTERNET=0
WINUAE=0
BOARD=a2065

while getopts "b:A:x:a:P:B:T:t:m:N:EIW" opt; do
    case "$opt" in
        b) BUILD="$OPTARG" ;;
        A) ARCHIVE="$OPTARG" ;;
        x) CLIENT="$OPTARG" ;;
        a) AMISSL="$OPTARG" ;;
        P) BASE_PORT="$OPTARG" ;;
        B) BULK="$OPTARG" ;;
        T) TAG="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        m) MODEL="$OPTARG" ;;
        N) BOARD="$OPTARG"; WINUAE=1 ;;
        E) ENFORCE=1 ;;
        I) INTERNET=1 ;;
        W) WINUAE=1 ;;
        *) sed -n '3,30p' "$0" >&2; exit 2 ;;
    esac
done

# The peer's address as the guest sees it.  10.0.2.2 is SLIRP's host under
# FS-UAE; under WinUAE the emulator runs elsewhere and has to come back over
# the LAN.
if [ "$WINUAE" = "1" ]; then
    PEER_IP="${AMINETXDUO_PEER_IP:?-W needs AMINETXDUO_PEER_IP set to the LAN address of this machine}"
    PEER_BIND=0.0.0.0
else
    PEER_IP="${AMINETXDUO_PEER_IP:-10.0.2.2}"
    PEER_BIND=127.0.0.1
fi

case "$BUILD" in /*) ;; *) BUILD="$ROOT/$BUILD" ;; esac
[ -n "$TAG" ] || TAG="legacy-$(basename "$BUILD")"

SMALL=1024
HOSTNAME_ALIAS=peer.localdomain

# ------------------------------------------------------------- the client --

# Extracted once into build/, from the archive, so the staging is a step
# somebody can repeat rather than a directory somebody assembled by hand.
if [ -z "$CLIENT" ]; then
    [ -f "$ARCHIVE" ] || {
        echo "no client archive at $ARCHIVE (-A FILE)" >&2; exit 2; }
    command -v lha >/dev/null || {
        echo "lha not found; unpack $ARCHIVE yourself and pass -x" >&2; exit 2; }

    XDIR="$ROOT/build/legacy-client/$(basename "${ARCHIVE%.lha}")"
    if [ ! -d "$XDIR" ]; then
        mkdir -p "$XDIR"
        ( cd "$XDIR" && lha -xq "$ARCHIVE" ) || {
            echo "could not unpack $ARCHIVE" >&2; exit 2; }
    fi
    # .020 is the one that runs on the A1200 profile's 68EC020.
    CLIENT=$(find "$XDIR" -name "curl.020" -type f | head -1)
    [ -n "$CLIENT" ] || CLIENT=$(find "$XDIR" -name "curl" -type f | head -1)
fi
[ -n "$CLIENT" ] && [ -f "$CLIENT" ] || { echo "no client binary" >&2; exit 2; }

BSD="$BUILD/src/bsdsocket/bsdsocket.library"
UG="$BUILD/src/usergroup/usergroup.library"
ADDIF="$BUILD/src/tools/AddNetInterface"
SHOWSTATUS="$BUILD/src/tools/ShowNetStatus"
for f in "$BSD" "$ADDIF"; do
    [ -f "$f" ] || { echo "missing $f" >&2; exit 2; }
done

. "$ROOT/tools/sana2-stage.sh"

A2065="${AMINETXDUO_A2065:-}"
if [ -z "$A2065" ] && [ "$BOARD" = a2065 ]; then
    for c in "$ROOT/build/a2065.device" \
             "$HOME/amiga-os-src/os-source/other_networking/sana2/bin/devs/a2065.device"; do
        [ -f "$c" ] && { A2065="$c"; break; }
    done
    [ -n "$A2065" ] && [ -f "$A2065" ] || {
        echo "No a2065.device found. Set AMINETXDUO_A2065=<path>." >&2; exit 2; }
fi

. "$ROOT/tools/amiga-toolchain.sh"
DRIVER="$ROOT/build/compare/CheckRunner"
mkdir -p "$ROOT/build/compare"
if [ ! -x "$DRIVER" ] || [ "$ROOT/tests/compare/checkrunner.c" -nt "$DRIVER" ]; then
    echo "==> building CheckRunner"
    "$AMIGA_GCC" -O2 -m68020 -fomit-frame-pointer -Wall -I"$AMIGA_NDK" \
                 -o "$DRIVER" "$ROOT/tests/compare/checkrunner.c"
fi

# ---------------------------------------------------------------- staging --

STAGE="$ROOT/build/legacy-stage-$TAG"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs" "$STAGE/d" "$STAGE/w"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
[ -z "$A2065" ] || cp "$A2065" "$STAGE/devs/a2065.device"
sana2_stage "$BOARD" "$STAGE/devs"

# The name the client is asked to resolve.  A hosts entry keeps the run off the
# internet and still goes through gethostbyname(), which is the call that would
# hand a 16-byte address to a client expecting 4.
printf '%s\t%s peer\n' "$PEER_IP" "$HOSTNAME_ALIAS" >> "$STAGE/devs/Internet/hosts"

cp "$BSD" "$STAGE/libs/bsdsocket.library"
[ -f "$UG" ] && cp "$UG" "$STAGE/libs/usergroup.library"
cp "$CLIENT" "$STAGE/curl"
cp "$ADDIF"  "$STAGE/AddNetInterface"
[ -f "$SHOWSTATUS" ] && cp "$SHOWSTATUS" "$STAGE/ShowNetStatus"

# clib2's startup opens both maths libraries before main(), and they have to be
# a matched pair or the client sits there forever.
for lib in mathieeedoubbas mathieeedoubtrans; do
    for c in "$ROOT/build/amissl-mathlibs/$lib.library" \
             "$ROOT/build/$lib-os.library" "$ROOT/build/$lib.library"; do
        [ -f "$c" ] && { cp "$c" "$STAGE/libs/$lib.library"; break; }
    done
    [ -f "$STAGE/libs/$lib.library" ] || {
        echo "no $lib.library; a clib2 client will not start" >&2; exit 2; }
done

# AmiSSL, opened before main() as well.  Nothing here uses https:, but the
# client does not know that.
if [ -d "$AMISSL/Libs/AmigaOS3" ]; then
    A3="$AMISSL/Libs/AmigaOS3"
    cp "$A3/amisslmaster.library" "$STAGE/libs/amisslmaster.library"
    mkdir -p "$STAGE/libs/AmiSSL" "$STAGE/AmiSSL/Libs/AmiSSL"
    cp "$A3"/AmiSSL/68020-40/*.library "$STAGE/libs/AmiSSL/"
    cp "$A3"/AmiSSL/68020-40/*.library "$STAGE/AmiSSL/Libs/AmiSSL/"
elif [ -d "$ROOT/build/amissl-stage/libs" ]; then
    cp -R "$ROOT/build/amissl-stage/libs"/* "$STAGE/libs/"
else
    echo "!! no AmiSSL staged; an AmiSSL-linked client will not start" >&2
fi

# ------------------------------------------------------------------ plan ----

W='-w "http=%{http_code} size=%{size_download} time=%{time_total} speed=%{speed_download} ip=%{remote_ip}:%{remote_port}\n"'
CURL="DH0:curl -s -S --max-time 120"
HOSTPORT="$PEER_IP:$BASE_PORT"
NAMEPORT="$HOSTNAME_ALIAS:$BASE_PORT"

PLAN="$STAGE/checks.txt"
{
    printf '# tests/compare/run-legacy-client.sh -b %s\n' "$BUILD"
    printf 'addif\tDH0:AddNetInterface DEVS:NetInterfaces/eth0\n'
    if [ -f "$STAGE/ShowNetStatus" ]; then
        printf 'status\tDH0:ShowNetStatus ALL\n'
    fi
    printf 'version\tDH0:curl --version\n'
    printf 'lit_small\t%s %s -o DH0:d/lit_small http://%s/bytes/%s\n' \
           "$CURL" "$W" "$HOSTPORT" "$SMALL"
    printf 'lit_bulk\t%s %s -o DH0:d/lit_bulk http://%s/bytes/%s\n' \
           "$CURL" "$W" "$HOSTPORT" "$BULK"
    # Three bulk transfers, not one.  Run-to-run spread on the same build is
    # about a percent, which is smaller than the gap between some cards, so a
    # single sample cannot be compared against another card's single sample.
    printf 'lit_bulk2\t%s %s -o DH0:d/lit_bulk2 http://%s/bytes/%s\n' \
           "$CURL" "$W" "$HOSTPORT" "$BULK"
    printf 'lit_bulk3\t%s %s -o DH0:d/lit_bulk3 http://%s/bytes/%s\n' \
           "$CURL" "$W" "$HOSTPORT" "$BULK"
    printf 'name_small\t%s %s -o DH0:d/name_small http://%s/bytes/%s\n' \
           "$CURL" "$W" "$NAMEPORT" "$SMALL"
    printf 'name_bulk\t%s %s -o DH0:d/name_bulk http://%s/bytes/%s\n' \
           "$CURL" "$W" "$NAMEPORT" "$BULK"
    # --interface goes through SIOCGIFCONF, the other place a library could
    # hand back an address family a 1990s client cannot read.
    printf 'iface\t%s %s --interface eth0 -o DH0:d/iface http://%s/bytes/%s\n' \
           "$CURL" "$W" "$HOSTPORT" "$SMALL"
    # A name with an A and a AAAA record, resolved for real through the SLIRP
    # forwarder.  Not hermetic, hence -I.
    if [ "$INTERNET" = "1" ]; then
        printf 'dns_dual\t%s %s -o DH0:d/dns_dual http://example.com/\n' \
               "$CURL" "$W"
    fi
} > "$PLAN"

STAGED=("$STAGE/devs" "$STAGE/libs" "$STAGE/d" "$STAGE/w" "$STAGE/curl"
        "$STAGE/AddNetInterface" "$PLAN")
[ -f "$STAGE/ShowNetStatus" ] && STAGED+=("$STAGE/ShowNetStatus")
[ -d "$STAGE/AmiSSL" ] && STAGED+=("$STAGE/AmiSSL")

# ------------------------------------------------------------------ peer ----

PEER_PID=""
cleanup() { [ -z "$PEER_PID" ] || kill -TERM "$PEER_PID" 2>/dev/null || true; }
trap cleanup EXIT INT TERM HUP

python3 "$ROOT/tests/peer/httppeer.py" --base-port "$BASE_PORT" \
        --bind "$PEER_BIND" --advertise "$PEER_IP" \
        --log "$ROOT/build/legacypeer-$TAG.log" \
        --seconds "$((TIMEOUT + 300))" \
        > "$ROOT/build/legacypeer-$TAG.out" 2>&1 &
PEER_PID=$!
sleep 2
kill -0 "$PEER_PID" 2>/dev/null || {
    echo "the host peer did not start:" >&2
    cat "$ROOT/build/legacypeer-$TAG.out" >&2; exit 2; }

# ------------------------------------------------------------------- run ----

export AMINETXDUO_RUN_TAG="$TAG"

# FS-UAE's own bsdsocket.library emulation would otherwise answer for the
# library under test.  It is only switchable from the base directory.
BASEDIR="$ROOT/build/fsuae-base-$TAG"
mkdir -p "$BASEDIR/Configurations"
cat > "$BASEDIR/Configurations/Host.fs-uae" <<'EOF'
[fs-uae]
floppy_drive_volume = 0
floppy_drive_volume_empty = 0
bsdsocket_library = 0
EOF

echo "==> library: $BSD"
echo "==> client:  $CLIENT"
echo "==> peer:    $PEER_BIND:$BASE_PORT, guest sees $PEER_IP"
[ "$WINUAE" = "0" ] || echo "==> card:    $BOARD, $SANA2_DRIVER"
echo "==> plan:"
sed 's/^/      /' "$PLAN"

set +e
if [ "$WINUAE" = "1" ]; then
    "$ROOT/tools/winuae-run.sh" -N "$BOARD" -m "$MODEL" -t "$TIMEOUT" \
        "$DRIVER" "${STAGED[@]}" > "$ROOT/build/legacy-$TAG.log" 2>&1
elif [ "$ENFORCE" = "1" ]; then
    "$ROOT/tools/enforcer-run.sh" -n -t "$TIMEOUT" -T "$TAG" \
        "$DRIVER" "${STAGED[@]}" > "$ROOT/build/legacy-$TAG.log" 2>&1
else
    "$ROOT/tools/fsuae-run.sh" -n -m "$MODEL" -t "$TIMEOUT" \
        "$DRIVER" "${STAGED[@]}" > "$ROOT/build/legacy-$TAG.log" 2>&1
fi
RC=$?
set -e

cleanup
PEER_PID=""

# ---------------------------------------------------------------- verdict ---

if [ "$WINUAE" = "1" ]; then
    HD="$ROOT/build/winuae-testhd-$TAG"
else
    HD="$ROOT/build/testhd-$TAG"
fi

echo
echo "================ $TAG ================"
[ -f "$HD/results.txt" ] && { echo "-- results (name rc ticks availmem)"
                              cat "$HD/results.txt"; }
for f in "$HD"/w/*.txt; do
    [ -f "$f" ] || continue
    echo "-- $(basename "$f")"
    tr -d '\r' < "$f" | head -40
done

echo "-- bytes"
python3 - "$ROOT/tests/peer" "$HD/d" "$SMALL" "$BULK" <<'PY'
import hashlib
import os
import sys

sys.path.insert(0, sys.argv[1])
from httppeer import master

d, small, bulk = sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
want = {"lit_small": small, "name_small": small, "iface": small,
        "lit_bulk": bulk, "lit_bulk2": bulk, "lit_bulk3": bulk,
        "name_bulk": bulk}
bad = 0
for name, n in sorted(want.items()):
    path = os.path.join(d, name)
    if not os.path.exists(path):
        print("   %-11s MISSING" % name)
        bad += 1
        continue
    got = open(path, "rb").read()
    ref = master(n)
    if got == ref:
        print("   %-11s %7d bytes  sha256 %s  OK"
              % (name, len(got), hashlib.sha256(got).hexdigest()[:16]))
    else:
        print("   %-11s %7d bytes (wanted %d)  sha256 %s  MISMATCH"
              % (name, len(got), n, hashlib.sha256(got).hexdigest()[:16]))
        bad += 1
print("   %d of %d verified" % (len(want) - bad, len(want)))
PY

if grep -qE "^(LONG|WORD|BYTE)-(READ|WRITE)" "$ROOT/build/serial-$TAG.log" 2>/dev/null; then
    echo "-- Enforcer hits"
    grep -E "^(LONG|WORD|BYTE)-(READ|WRITE)" "$ROOT/build/serial-$TAG.log" | head -20
fi

echo
echo "emulator log: build/legacy-$TAG.log"
echo "guest files:  $HD"
exit "$RC"
