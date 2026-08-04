#!/usr/bin/env bash
#
# Run the `fetch` command against real URLs, under FS-UAE on SLIRP.
#
#   tests/tls/run-fetch.sh [-m MODEL] [-t SECONDS] [-c CPU] [-k MHZ] [-b BUILDDIR]
#
# tools/fsuae-run.sh starts ONE executable with no arguments, and `fetch` is a
# command that takes arguments, so ToolsSmoke stands in the middle: it reads
# DH0:commands.txt and runs each line through SystemTagList() with the output
# redirected into DH0:tools.txt, which the harness prints back.  Same shape as
# the tools smoke run; only the command list and the staging differ.
#
# This is the TRAVELLER test.  tests/tls/run-api.sh proves tls.library works
# for a program written against it; this proves the command we ship uses it,
# over http: as well as https:, and that the failures a user will actually
# meet, a certificate for somebody else, a scheme nobody supports, are
# legible rather than mysterious.
#
# NOT A BASELINE, for the same reasons as run-api.sh: it depends on the
# internet, on FS-UAE's SLIRP NAT, on third parties' servers, and on
# certificates that rotate.
#
# The a2065.device driver is not ours to ship: point AMINETXDUO_A2065 at one,
# or drop a copy in build/a2065.device.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
MODEL=A1200
TIMEOUT=420
CPU=""
CLOCK=""
BUILD="${AMINETXDUO_BUILD:-build/tls}"

while getopts "m:t:c:k:b:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        c) CPU="$OPTARG" ;;
        k) CLOCK="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        *) echo "usage: $0 [-m model] [-t seconds] [-c cpu] [-k MHz] [-b builddir]" >&2; exit 2 ;;
    esac
done

SMOKE="$ROOT/$BUILD/src/tools/ToolsSmoke"
FETCH="$ROOT/$BUILD/src/tools/fetch"
ADDIF="$ROOT/$BUILD/src/tools/AddNetInterface"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"
TLS="$ROOT/$BUILD/src/tlslib/tls.library"
STORE="$ROOT/$BUILD/certificates"

for f in "$SMOKE" "$FETCH" "$ADDIF" "$BSD"; do
    [ -f "$f" ] || { echo "missing $f, build the tree first" >&2; exit 2; }
done

# tls.library and the trust store are optional here on purpose: without them
# the https: lines must still fail LEGIBLY, and that is worth being able to
# run.  With them, they must succeed.
HAVE_TLS=0
if [ -f "$TLS" ] && [ -f "$STORE" ]; then
    HAVE_TLS=1
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

STAGE="$ROOT/build/fetch-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$A2065" "$STAGE/devs/a2065.device"
cp "$BSD"   "$STAGE/libs/bsdsocket.library"
cp "$FETCH" "$STAGE/fetch"
cp "$ADDIF" "$STAGE/AddNetInterface"

if [ "$HAVE_TLS" = "1" ]; then
    cp "$TLS"   "$STAGE/libs/tls.library"
    cp "$STORE" "$STAGE/devs/Internet/certificates"
    echo "==> tls.library staged, trust store $(wc -c < "$STORE" | tr -d ' ') bytes"
else
    echo "==> NO tls.library in $BUILD, the https: lines must fail legibly"
fi

# One interface up front, so the stack comes up once and stays up: every
# `fetch` after it finds bsdsocket.library already open.  Without this each
# command would start and stop the whole stack, DHCP lease included.
#
# The URLs:
#   example.com        the plainest 200 there is, over http:
#   tls-v1-2.badssl.com  answers 301 to an absolute URL on a NON-DEFAULT PORT,
#                      which exercises the redirect follow and the ":1012" in
#                      the URL parser at the same time
#   ecc256.badssl.com  an ECDSA leaf, so the P-256 verify path is covered too
#   wrong.host.badssl.com  a valid chain for somebody else, must be REFUSED
#   ftp://             a scheme this command does not do
#
# DELIBERATELY ABSENT: https://example.com/, https://www.iana.org/ and every
# other Cloudflare-fronted host.  They send three- and four-certificate chains,
# which at 14 MHz take longer to verify than their front end is willing to wait
# for a ClientKeyExchange, so they fail with "the connection is closed" through
# no fault of ours and would make this run look broken.  Not a crash and not a
# library defect, that was the harness losing the EMULATOR to SIGPIPE; see
# docs/RESEARCH.md.  www.iana.org completes in 11.3 s at -k 28.
if [ -n "${AMINETXDUO_FETCH_COMMANDS:-}" ]; then
    cp "$AMINETXDUO_FETCH_COMMANDS" "$STAGE/commands.txt"
    echo "==> command list: $AMINETXDUO_FETCH_COMMANDS"
else
cat > "$STAGE/commands.txt" <<'EOF'
# the argument template, via ReadArgs' own "?"
SYS:fetch ?
SYS:AddNetInterface eth0
SYS:fetch http://example.com/
SYS:fetch http://example.com/ TO DH0:plain.txt
SYS:fetch https://tls-v1-2.badssl.com/ TO DH0:redirect.txt
SYS:fetch https://ecc256.badssl.com/ TO DH0:ecdsa.txt
SYS:fetch https://wrong.host.badssl.com/ TO DH0:refused.txt
SYS:fetch ftp://example.com/
SYS:fetch http://example.invalid/
EOF
fi

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-fetch}"

CPUARG=()
[ -z "$CPU" ]   || CPUARG+=(-c "$CPU")
[ -z "$CLOCK" ] || CPUARG+=(-k "$CLOCK")

exec "$ROOT/tools/fsuae-run.sh" -n -m "$MODEL" -t "$TIMEOUT" "${CPUARG[@]}" \
     "$SMOKE" "$STAGE/devs" "$STAGE/libs" "$STAGE/fetch" \
     "$STAGE/AddNetInterface" "$STAGE/commands.txt"
