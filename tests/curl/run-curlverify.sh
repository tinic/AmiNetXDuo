#!/usr/bin/env bash
#
# The curl verification suite: run curl against bsdsocket.library, hard.
#
#   tests/curl/run-curlverify.sh [-m MODEL] [-t SECS] [-c CPU] [-k MHZ]
#                                [-b STACKBUILD] [-C CURLBUILD] [-g GROUPS]
#                                [-x CURLBINARY] [-A AMISSLDIR] [-T TAG]
#                                [-P BASEPORT] [-p SWEEP] [-n SUBSTRING]
#
#   -g   which groups to run.  Default ABCDEF, all hermetic.
#          A HTTP mechanics   B connections/multi   C failure paths
#          D resource use     E TLS                 F FTP
#          G the internet -- NOT A BASELINE, and it is on its own because of
#            that: -g G runs only the internet cases and nothing else.
#   -x   run somebody ELSE'S curl binary instead of ours.  The point of the
#        suite is the stack, and a binary nobody here built cannot have been
#        shaped around our quirks.
#   -P   base port for the host peer (default 7100; it uses base+0..base+99).
#   -p   run a concurrency SWEEP instead of the suite: a comma-separated list
#        of --parallel-max values.  For chasing the cliff the suite finds.
#   -n   run only the cases whose name contains this substring.
#   -A   stage an AmiSSL installation from this directory (the extracted
#        util/libs/AmiSSL-v5-OS3.lha).  Only a third-party curl needs it: ours
#        goes through tls.library.  The driver makes the AmiSSL: assign.
#
# HERMETIC BY DEFAULT
#
#   Groups A to F talk to tests/curl/curlpeer.py on this host and to nothing
#   else -- no DNS beyond one deliberate NXDOMAIN, no internet, no public CA.
#   The TLS half runs against tests/curl/mkpki.sh's own root, handed to curl
#   as --cacert, so chain depth 2/3/4 and RSA/ECDSA are properties of files in
#   build/curl-pki rather than of whatever badssl.com is doing today.
#
#   -g G is the opposite and says so: real hosts, real CAs, real weather.
#   NOT A BASELINE.
#
# WHAT IS BEING TESTED
#
#   bsdsocket.library.  curl is the adversary: every failure is a bug in the
#   stack until somebody proves otherwise.  See tests/curl/curlsuite.py, which
#   holds the cases and the scoring, and docs/RESEARCH.md.
#
# THE STACK BUILD MATTERS FOR https:
#
#   -b names the build the libraries come from and defaults to build/tls,
#   because that is the one configured with AMINETXDUO_TLS=ON.  Without it
#   there is no private context vector for tls.library to borrow NetX Duo
#   through and every https: case fails legibly.
#
# The a2065.device driver is not ours to ship: point AMINETXDUO_A2065 at one,
# or drop a copy in build/a2065.device.  mathieeedoubbas.library likewise --
# curl uses doubles, newlib implements them by calling that library, and it is
# not in Kickstart 3.1 ROM.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
MODEL=A1200
TIMEOUT=2400
CPU=""
CLOCK=""
STACK_BUILD="${AMINETXDUO_BUILD:-build/tls}"
CURL_BUILD="build/curl-tls"
SUITE_GROUPS="ABCDEF"
FOREIGN=""
TAG=""
BASE_PORT=7100
AMISSL=""
PROBE=""
ONLY=""

while getopts "m:t:c:k:b:C:g:x:T:P:A:p:n:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        c) CPU="$OPTARG" ;;
        k) CLOCK="$OPTARG" ;;
        b) STACK_BUILD="$OPTARG" ;;
        C) CURL_BUILD="$OPTARG" ;;
        g) SUITE_GROUPS="$OPTARG" ;;
        x) FOREIGN="$OPTARG" ;;
        T) TAG="$OPTARG" ;;
        P) BASE_PORT="$OPTARG" ;;
        A) AMISSL="$OPTARG" ;;
        p) PROBE="$OPTARG" ;;
        n) ONLY="$OPTARG" ;;
        *) echo "usage: $0 [-m model] [-t secs] [-c cpu] [-k MHz] [-b build]" \
                "[-C curlbuild] [-g GROUPS] [-x curlbinary] [-T tag]" >&2
           exit 2 ;;
    esac
done

[ -n "$TAG" ] || TAG="curlverify"

BSD="$ROOT/$STACK_BUILD/src/bsdsocket/bsdsocket.library"
ADDIF="$ROOT/$STACK_BUILD/src/tools/AddNetInterface"
TLS="$ROOT/$STACK_BUILD/src/tlslib/tls.library"
UG="$ROOT/$STACK_BUILD/src/usergroup/usergroup.library"
STORE="$ROOT/$STACK_BUILD/certificates"

if [ -n "$FOREIGN" ]; then
    CURL="$FOREIGN"
else
    CURL="$ROOT/$CURL_BUILD/src/curl"
fi

for f in "$CURL" "$BSD" "$ADDIF"; do
    [ -f "$f" ] || { echo "missing $f -- build it first" >&2; exit 2; }
done

# ------------------------------------------------------------- the PKI ----

PKI="$ROOT/build/curl-pki"
"$ROOT/tests/curl/mkpki.sh" "$PKI" >/dev/null || {
    echo "tests/curl/mkpki.sh failed" >&2; exit 2; }

# ------------------------------------------------------------ the driver --

. "$ROOT/tools/amiga-toolchain.sh"

DRIVER="$ROOT/build/curl/CurlCheck"
mkdir -p "$ROOT/build/curl"
if [ ! -x "$DRIVER" ] || [ "$ROOT/tests/curl/curlcheck.c" -nt "$DRIVER" ]; then
    echo "==> building CurlCheck"
    "$AMIGA_GCC" -O2 -m68020 -fomit-frame-pointer -Wall -I"$AMIGA_NDK" \
                 -o "$DRIVER" "$ROOT/tests/curl/curlcheck.c"
fi

# ------------------------------------------------------------- a2065 ------

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
    echo "No a2065.device found. Set AMINETXDUO_A2065=<path>." >&2; exit 2; }

MATH="${AMINETXDUO_MATHIEEEDOUBBAS:-}"
if [ -z "$MATH" ]; then
    for candidate in \
        "$ROOT/build/mathieeedoubbas.library" \
        "$HOME/amigaos/libs/mathieeedoubbas.library"
    do
        [ -f "$candidate" ] && { MATH="$candidate"; break; }
    done
fi

# mathieeedoubtrans.library is NOT optional for a clib2-built client, and the
# failure is total rather than partial: clib2's startup opens it before main()
# and prints "mathieeedoubtrans.library could not be opened." if it cannot.
# Our own curl is newlib and never asks for it, which is why nothing here
# needed it until somebody else's binary was run.  Every Workbench install has
# it in LIBS:; a bare directory hard drive does not.
MATHTRANS="${AMINETXDUO_MATHIEEEDOUBTRANS:-}"
if [ -z "$MATHTRANS" ]; then
    for candidate in \
        "$ROOT/build/mathieeedoubtrans-os.library" \
        "$ROOT/build/mathieeedoubtrans.library" \
        "$HOME/amigaos/libs/mathieeedoubtrans.library"
    do
        [ -f "$candidate" ] && { MATHTRANS="$candidate"; break; }
    done
fi

# ------------------------------------------------------------- staging ----

STAGE="$ROOT/build/curlverify-stage-$TAG"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs" "$STAGE/d" "$STAGE/w"
for g in g1 g2 g3 g4; do mkdir -p "$STAGE/$g"; done
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$A2065" "$STAGE/devs/a2065.device"
cp "$BSD"   "$STAGE/libs/bsdsocket.library"
cp "$CURL"  "$STAGE/curl"
cp "$ADDIF" "$STAGE/AddNetInterface"
# usergroup.library is optional for OUR curl and REQUIRED by a third-party one
# built against clib2, which opens it at startup.
if [ -f "$UG" ]; then
    cp "$UG" "$STAGE/libs/usergroup.library"
else
    echo "==> no usergroup.library in $STACK_BUILD (ours does not need it)"
fi

if [ -f "$TLS" ] && [ -f "$STORE" ]; then
    mkdir -p "$STAGE/devs/Internet"
    cp "$TLS"   "$STAGE/libs/tls.library"
    cp "$STORE" "$STAGE/devs/Internet/certificates"
else
    echo "==> NO tls.library in $STACK_BUILD -- every https: case must fail"
fi

if [ -n "$MATH" ] && [ -f "$MATH" ]; then
    cp "$MATH" "$STAGE/libs/mathieeedoubbas.library"
else
    echo "!! NO mathieeedoubbas.library staged; curl will not start." >&2
fi

if [ -n "$MATHTRANS" ] && [ -f "$MATHTRANS" ]; then
    cp "$MATHTRANS" "$STAGE/libs/mathieeedoubtrans.library"
elif [ -n "$FOREIGN" ]; then
    echo "!! NO mathieeedoubtrans.library staged.  A clib2-built client opens" >&2
    echo "   it before main() and will not start.  Set" >&2
    echo "   AMINETXDUO_MATHIEEEDOUBTRANS=<path> or drop one in build/." >&2
fi

# AmiSSL, for a third-party curl that does its TLS through it.  The layout is
# the one amisslmaster.library expects after AmiSSL's own installer has run:
# LIBS:amisslmaster.library, and the versioned library for THIS cpu flattened
# into AmiSSL:Libs/AmiSSL/.  The archive keeps 68020-40 and 68060 apart; the
# A1200 profile is a 68EC020, so the first is the one that runs here.
if [ -n "$AMISSL" ]; then
    A3="$AMISSL/Libs/AmigaOS3"
    [ -d "$A3" ] || { echo "no $A3 -- is that an extracted AmiSSL-v5-OS3?" >&2
                      exit 2; }
    cp "$A3/amisslmaster.library" "$STAGE/libs/amisslmaster.library"
    # Both conventions, because the archive documents one ("usually stored
    # inside the AmiSSL:Libs/AmiSSL directory") and AmiSSL's own installer is
    # free to use the other.  Two copies of a 3.5 MB library on a host
    # directory cost nothing and remove a variable.
    mkdir -p "$STAGE/AmiSSL/Libs/AmiSSL" "$STAGE/libs/AmiSSL"
    cp "$A3"/AmiSSL/68020-40/*.library "$STAGE/AmiSSL/Libs/AmiSSL/"
    cp "$A3"/AmiSSL/68020-40/*.library "$STAGE/libs/AmiSSL/"
    if [ -d "$AMISSL/Certs" ]; then
        cp -R "$AMISSL/Certs" "$STAGE/AmiSSL/Certs"
    fi
    cp "$PKI/testroots.pem" "$STAGE/AmiSSL/testroots.pem"
    echo "==> AmiSSL staged: $(ls "$STAGE/AmiSSL/Libs/AmiSSL")"
fi

# The test PKI, in both shapes: tls.library reads the indexed store, and a
# third-party curl over AmiSSL would want the PEM.
cp "$PKI/teststore"    "$STAGE/teststore"
cp "$PKI/otherstore"   "$STAGE/otherstore"
cp "$PKI/testroots.pem" "$STAGE/testroots.pem"

# What the upload cases send, generated from the same seeded buffer the
# server checks against.
python3 - "$ROOT/tests/curl" "$STAGE" <<'PY'
import os
import sys

sys.path.insert(0, sys.argv[1])
from curlpeer import master

stage = sys.argv[2]
open(os.path.join(stage, "up4k.bin"), "wb").write(master(4096))
open(os.path.join(stage, "up200k.bin"), "wb").write(master(204800))
PY

CACERT="--cacert DH0:teststore"
[ -z "$FOREIGN" ] || CACERT="--cacert DH0:testroots.pem"

SELECT=()
[ -z "$PROBE" ] || SELECT+=(--probe "$PROBE")
[ -z "$ONLY" ]  || SELECT+=(--only "$ONLY")

# The internet suite has no seeded buffer to check its big download against,
# so the reference is fetched HERE, once, and the two are hashed.  A case that
# claims 657,797 bytes arrived and never looks at them is not a test.
case "$SUITE_GROUPS" in
    *G*)
        REF="$ROOT/build/curl-internet-reference.bin"
        if [ ! -s "$REF" ]; then
            echo "==> fetching the internet suite's reference copy"
            curl -sS -o "$REF" \
                 "http://ftp.fau.de/aminet/comm/tcp/AmiTCP-SDK-4.3.lha" \
                || echo "!! could not fetch it; that case will report so" >&2
        fi
        [ -s "$REF" ] && SELECT+=(--reference "$REF")
        ;;
esac

python3 "$ROOT/tests/curl/curlsuite.py" --emit "$STAGE/checks.txt" \
        --groups "$SUITE_GROUPS" --base-port "$BASE_PORT" --cacert "$CACERT" \
        "${SELECT[@]}"

# ---------------------------------------------------------------- peers ---

PEERLOG="$ROOT/build/curlpeer-$TAG.log"
PEER_ARGS=(--base-port "$BASE_PORT" --log "$PEERLOG"
           --seconds "$((TIMEOUT + 300))")
case "$SUITE_GROUPS" in
    *G*) ;;                             # the internet suite needs no peer PKI
    *)   PEER_ARGS+=(--pki "$PKI") ;;
esac

python3 "$ROOT/tests/curl/curlpeer.py" "${PEER_ARGS[@]}" \
        > "$ROOT/build/curlpeer-$TAG.out" 2>&1 &
PEER_PID=$!
trap 'kill -TERM "$PEER_PID" 2>/dev/null || true' EXIT INT TERM HUP

sleep 2
kill -0 "$PEER_PID" 2>/dev/null || {
    echo "curlpeer.py did not start:" >&2
    cat "$ROOT/build/curlpeer-$TAG.out" >&2
    exit 2
}

# ---------------------------------------------------------------- slirp ---
#
# Active-mode FTP is the one case where the HOST has to reach the GUEST, and
# SLIRP is a NAT.  The forward is the same mechanism tests/tools/
# run-nettools.sh uses, on the port curl is told to bind with -P.
# Four ports, not one: curl binds a fresh one per data connection and the
# previous is still in TIME_WAIT, so a single-port range fails with (30).
REDIR=""
for p in 60 61 62 63; do
    d=$((BASE_PORT + p))
    [ -z "$REDIR" ] || REDIR="$REDIR;"
    REDIR="${REDIR}uae_slirp_redir = tcp:$d:$d"
done
export AMINETXDUO_FSUAE_EXTRA="$REDIR"
export AMINETXDUO_RUN_TAG="$TAG"

CPUARG=()
[ -z "$CPU" ]   || CPUARG+=(-c "$CPU")
[ -z "$CLOCK" ] || CPUARG+=(-k "$CLOCK")

echo "==> curl:  $CURL"
echo "==> stack: $STACK_BUILD"
echo "==> groups $SUITE_GROUPS, peer on 127.0.0.1:$BASE_PORT+"
case "$SUITE_GROUPS" in
    *G*) echo "==> NOT A BASELINE: this run depends on the internet" ;;
esac

set +e
"$ROOT/tools/fsuae-run.sh" -n -m "$MODEL" -t "$TIMEOUT" "${CPUARG[@]}" \
    "$DRIVER" "$STAGE/devs" "$STAGE/libs" "$STAGE/curl" \
    "$STAGE/AddNetInterface" "$STAGE/checks.txt" \
    "$STAGE/teststore" "$STAGE/otherstore" "$STAGE/testroots.pem" \
    "$STAGE/up4k.bin" "$STAGE/up200k.bin" \
    "$STAGE/d" "$STAGE/w" "$STAGE/g1" "$STAGE/g2" "$STAGE/g3" "$STAGE/g4" \
    ${AMISSL:+"$STAGE/AmiSSL"} \
    > "$ROOT/build/curlverify-$TAG.log" 2>&1
RC=$?
set -e

HD="$ROOT/build/testhd-$TAG"

echo
echo "================ the run ================"
sed -n '/---- curlcheck.txt ----/,$p' "$ROOT/build/curlverify-$TAG.log" \
    | head -400 || true
tail -3 "$ROOT/build/curlverify-$TAG.log"

echo
echo "================ scoring ================"
set +e
python3 "$ROOT/tests/curl/curlsuite.py" --score "$HD" --groups "$SUITE_GROUPS" \
        --base-port "$BASE_PORT" --cacert "$CACERT" "${SELECT[@]}"
SCORE=$?
set -e

echo
echo "full emulator log: build/curlverify-$TAG.log"
echo "peer log:          $PEERLOG"
echo "guest files:       $HD"

exit "$SCORE"
