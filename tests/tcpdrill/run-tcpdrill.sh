#!/usr/bin/env bash
#
# Run the tcpdrill scripts under an emulator.
#
#   tests/tcpdrill/run-tcpdrill.sh [-m MODEL] [-t SECS] [-b BUILD] [-T TAG]
#                                  [-s SCRIPT|all] [-A]
#
# -s all runs every script under scripts/, one boot each, and prints one line
# per file plus tcpdrill_all=PASS|FAIL.
#
# -A PICKS AMIBERRY.  FS-UAE needs an X server and dies in GLAD without one,
# so on a headless box, which the Amiga lab machine is, the run ends with
# "fs-uae exited early after 1s" and no results at all.  Amiberry runs
# genuinely headless.  Same block tests/ipv6/run-socket-fsuae.sh carries, and
# the same flag.
#
# ONE BOOT PER SCRIPT FILE.  Every case in the file runs inside a single
# emulator run, because build/.fsuae.lock serialises runs and the queue is
# deep.  Cases are independent, each opens its own socket and each uses a
# different peer ISN, so the only thing they share is the stack, which is
# the point.
#
# NO NETWORK.  There is no `-n` here and no a2065.device: the guest's only
# interface is tcpdrill.device, which TcpDrill creates in its own address
# space, so nothing in this test depends on FS-UAE's SLIRP, on the host's
# routing, or on anything outside the emulated machine.  See tests/tcpdrill/
# tapdev.h for why a host-side injector is not possible here at all.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
MODEL=A1200
TIMEOUT=300
BUILD="${AMINETXDUO_BUILD:-build/cm}"
TAG="tcpdrill"
SCRIPT="$ROOT/tests/tcpdrill/scripts/tcp.drill"
RUNNER="${AMINETXDUO_RUNNER:-fsuae}"

while getopts "m:t:b:T:s:A" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        T) TAG="$OPTARG" ;;
        s) SCRIPT="$OPTARG" ;;
        A) RUNNER=amiberry ;;
        *) echo "usage: $0 [-m model] [-t secs] [-b build] [-T tag] [-s script] [-A]" >&2
           exit 2 ;;
    esac
done

BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"
DRILL="$ROOT/$BUILD/tests/tcpdrill/TcpDrill"

# -s all runs every script, one boot each, and reports a table.  Not one boot
# for a concatenation of them: the scripts were written one file per boot and
# two of them reuse a local port, so joining them would test a listener state
# nothing else does.  Recursion rather than a loop around the body below, so
# each arm gets its own stage directory and its own guest files.
if [ "$SCRIPT" = all ]; then
    rc=0
    ARM_A=()
    [ "$RUNNER" = amiberry ] && ARM_A=(-A)

    for s in "$ROOT"/tests/tcpdrill/scripts/*.drill; do
        name=$(basename "$s" .drill)
        out="$ROOT/build/tcpdrill-$TAG-$name.out"

        # keepalive.drill is written against a build whose idle timer is five
        # seconds; against the shipping 7200 every probe case times out and is
        # right to.  Named rather than silently dropped, so the line below is
        # not read as a suite that covers it.
        if [ "$name" = keepalive ] &&
           ! grep -q '^AMINETXDUO_TCP_KEEPALIVE_INITIAL:STRING=[0-9]' \
                 "$ROOT/$BUILD/CMakeCache.txt" 2>/dev/null; then
            echo "$name: SKIP, needs -DAMINETXDUO_TCP_KEEPALIVE_INITIAL=5"
            continue
        fi

        arm=0
        "$0" -m "$MODEL" -t "$TIMEOUT" -b "$BUILD" -T "$TAG-$name" -s "$s" \
            "${ARM_A[@]+"${ARM_A[@]}"}" > "$out" 2>&1 || arm=$?

        verdict=$(grep -E 'case\(s\)' "$out" | tail -1)
        echo "$name: ${verdict:-no verdict} rc=$arm"
        [ "$arm" -eq 0 ] || rc=1
    done

    [ "$rc" -eq 0 ] && echo "tcpdrill_all=PASS" || echo "tcpdrill_all=FAIL"
    exit "$rc"
fi

for f in "$BSD" "$DRILL" "$SCRIPT"; do
    [ -f "$f" ] || { echo "missing $f, build it first" >&2; exit 2; }
done

STAGE="$ROOT/build/tcpdrill-stage-$TAG"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/tcpdrill/devs" "$STAGE/devs"
cp "$BSD"    "$STAGE/libs/bsdsocket.library"
cp "$DRILL"  "$STAGE/TcpDrill"
cp "$SCRIPT" "$STAGE/drill.txt"

echo "==> stack:  $BUILD"
echo "==> script: $SCRIPT ($(grep -c '^case ' "$SCRIPT") cases)"

export AMINETXDUO_RUN_TAG="$TAG"

# Both arms run amiberry-run.sh -- fs-uae is gone -- so the guest directory is
# the one amiberry-run.sh writes, whatever RUNNER says.  It did not used to be:
# the default RUNNER is still "fsuae", so the else arm looked in
# build/testhd-<tag>, found no tcpdrill.txt, printed "the run did not get that
# far" and exited 1.  A drill that passed 28 cases and 227 checks reported as a
# failure, which is worse than a drill that does not run.
RUN=("$ROOT/tools/amiberry-run.sh")
HD="$ROOT/build/amiberry-testhd-$TAG"

set +e
"${RUN[@]}" -m "$MODEL" -t "$TIMEOUT" \
    "$DRILL" "$STAGE/devs" "$STAGE/libs" "$STAGE/drill.txt" \
    > "$ROOT/build/tcpdrill-$TAG.log" 2>&1
RC=$?
set -e

echo
echo "================ tcpdrill ================"
if [ -f "$HD/tcpdrill.txt" ]; then
    cat "$HD/tcpdrill.txt"
else
    echo "(no tcpdrill.txt, the run did not get that far)"
    [ -f "$HD/stdout.txt" ] && { echo "--- stdout ---"; cat "$HD/stdout.txt"; }
fi

echo
echo "emulator log: build/tcpdrill-$TAG.log"
if [ "$RUNNER" = "amiberry" ]; then
    echo "serial log:   build/amiberry-serial-$TAG.log"
else
    echo "serial log:   build/serial-$TAG.log"
fi
echo "guest files:  $HD"

exit "$RC"
