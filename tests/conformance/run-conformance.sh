#!/usr/bin/env bash
#
# Run the bsdsocktest conformance suite against our bsdsocket.library.
#
#   tests/conformance/run-conformance.sh [-m MODEL] [-c CPU] [-t SECONDS]
#                                        [-T TAG] [-a "SUITE ARGS"] [-p] [-b BUILDDIR]
#
# It was called run-fsuae.sh and it has driven tools/amiberry-run.sh since
# fs-uae left the tree on 2026-08-04.  The name was the only thing left saying
# otherwise, and it said it in the one place that is read as a decision: the
# job in .github/workflows/emulator.yml was titled after it.  Named for what it
# tests now, like every other wired harness -- run-tls13.sh, run-addifup.sh,
# run-cardsweep.sh -- so that which emulator boots the guest stays a question
# tools/amiberry-run.sh answers, once, for all of them.
#
# -b (or AMINETXDUO_BUILD) picks the build tree the library comes from, so the
# floor build and an -DAMINETXDUO_IPV6=ON build can both be measured.
#
# -p runs conf_probe instead of the suite: the hand-written triage walk that
# prints rc and errno for every call, which is how you find out *why* a
# category collapsed.
#
# -a is the suite's own ReadArgs line, e.g.
#      -a "LOOPBACK NOPAGE"
#      -a "CATEGORY socket NOPAGE VERBOSE"
#      -a "HOST 10.0.2.2 NOPAGE"
#
# Stages LIBS:bsdsocket.library and LIBS:usergroup.library from build/cm,
# DEVS:a2065.device plus the netstack DEVS: config, the suite binary and the
# argument line, then boots tools/amiberry-run.sh with conf_launcher as the
# program the Startup-Sequence runs.  See conf_launcher.c for why there is a
# launcher at all.
#
# The emulator ships its own bsdsocket.library emulation, and if it answered
# OpenLibrary() the whole run would be measuring a host-socket shim instead of
# our stack.  Turning it off in the config is not how that is settled here: the
# emulator log says "bsdsocket.library installed" either way, that line is
# printed when the emulation is *built*, not when it is registered, so a config
# key is not the proof.  The proof is threefold and checked below / visible in
# the output:
#   1. Removing build/cm/.../bsdsocket.library from the staged LIBS: makes the
#      suite bail with "bsdsocket.library not available", so nothing else on
#      the machine answers OpenLibrary("bsdsocket.library", 4).
#   2. The TAP log's "# bsdsocket.library:" line reads AmiNetXDuo, our
#      SBTC_RELEASESTRPTR.  UAE's emulation answers "UAE <version>".
#   3. build/serial-<tag>.log fills with our ami_log netstack bring-up
#      (DHCP lease, resolver) as the library is opened.
#
# Results land in:
#   build/amiberry-testhd-<tag>/bsdsocktest.log  the TAP log, the result
#   build/amiberry-testhd-<tag>/conf-out.txt     the suite's console summary
#   build/serial-<tag>.log                       our ami_log output
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
MODEL=A1200
TIMEOUT=600
CPU=""
# -T wins, then AMINETXDUO_RUN_TAG from the environment, then the default. The
# environment has to be honoured here: this script exports AMINETXDUO_RUN_TAG
# to tools/amiberry-run.sh, so taking the default unconditionally used to
# OVERWRITE a caller's tag, two runs started with different
# AMINETXDUO_RUN_TAG values would silently share build/amiberry-testhd-conformance
# and clobber each other's results.
TAG="${AMINETXDUO_RUN_TAG:-conformance}"
ARGS="NOPAGE"
PROBE=0

while getopts "m:c:t:T:a:b:p" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        c) CPU="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        T) TAG="$OPTARG" ;;
        a) ARGS="$OPTARG" ;;
        b) AMINETXDUO_BUILD="$OPTARG" ;;
        p) PROBE=1 ;;
        *) echo "usage: $0 [-m model] [-c cpu] [-t secs] [-T tag] [-a args]" \
                "[-b builddir] [-p]" >&2
           exit 2 ;;
    esac
done

SUITE="$ROOT/build/bsdsocktest/bsdsocktest"
LAUNCHER="$ROOT/build/bsdsocktest/conf_launcher"
BUILD="${AMINETXDUO_BUILD:-build/cm}"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"
UG="$ROOT/$BUILD/src/usergroup/usergroup.library"

for f in "$SUITE" "$LAUNCHER"; do
    [ -f "$f" ] || { echo "missing $f, run tests/conformance/build.sh" >&2; exit 2; }
done
for f in "$BSD" "$UG"; do
    [ -f "$f" ] || { echo "missing $f, build bsdsocket_library usergroup_library" >&2; exit 2; }
done

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

STAGE="$ROOT/build/conf-stage-$TAG"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$A2065" "$STAGE/devs/a2065.device"
cp "$BSD" "$STAGE/libs/bsdsocket.library"
cp "$UG"  "$STAGE/libs/usergroup.library"
cp "$SUITE" "$STAGE/bsdsocktest"
printf '%s\n' "$ARGS" > "$STAGE/conf-args"

export AMINETXDUO_RUN_TAG="$TAG"

set +e
if [ "$PROBE" = "1" ]; then
    # The probe needs no arguments and no big stack, so it is run directly
    # instead of through the launcher.
    "$ROOT/tools/amiberry-run.sh" -N a2065 -m "$MODEL" ${CPU:+-c "$CPU"} -t "$TIMEOUT" \
        "$ROOT/build/bsdsocktest/conf_probe" "$STAGE/devs" "$STAGE/libs"
    status=$?
    set -e
    exit "$status"
fi
"$ROOT/tools/amiberry-run.sh" -N a2065 -m "$MODEL" ${CPU:+-c "$CPU"} -t "$TIMEOUT" \
    "$LAUNCHER" "$STAGE/devs" "$STAGE/libs" "$STAGE/bsdsocktest" \
    "$STAGE/conf-args"
status=$?
set -e

# ---- which library actually answered -------------------------------------
#
# A run against somebody else's bsdsocket.library says nothing about ours,
# whatever its TAP output claims.  This used to print "results are meaningless"
# and then exit with the guest's status, so a green run against a foreign
# library was a pass.  A run that cannot be attributed is not a result: 3, its
# own status, distinct from the suite failing.
echo "---- stack under test ----"
ident=$(grep -m1 "^# bsdsocket.library:" \
        "$ROOT/build/amiberry-testhd-$TAG/bsdsocktest.log" 2>/dev/null || true)
case "$ident" in
    *AmiNetXDuo*)
        echo "$ident  (ours)"
        ;;
    "")
        echo "!! no stack identification in the TAP log, so nothing says which" >&2
        echo "!! library answered and the results cannot be attributed." >&2
        echo "conformance: NOT MEASURED (no stack identification)" >&2
        exit 3
        ;;
    *)
        echo "!! $ident" >&2
        echo "!! That is NOT our library. Whatever the suite reported, it" >&2
        echo "!! reported it about somebody else's stack." >&2
        echo "conformance: NOT MEASURED (a foreign bsdsocket.library answered)" >&2
        exit 3
        ;;
esac

# ---- the TAP log is the result -------------------------------------------
#
# `exit "$status"` alone was a pass on every run that reached the end.  $status
# is the emulator's, which is the guest's, which is conf_launcher.c:134 --
# `return RETURN_OK;` unconditional, with the comment "hand the harness a
# success so the run is scored from the TAP log".  Nothing scored the TAP log.
# The only read of it above greps one line for attribution.  So the whole
# bsdsocktest conformance suite, as .github/workflows/emulator.yml runs it, was
# green however many conformance tests failed.
#
# The scorer is in tests/conformance/tap-verdict.sh so that
# tests/conformance/tap-verdict-selftest.sh can prove it goes red without a
# ROM, a driver or the submodule.  It returns 3 for "not measured", which is
# the same distinction the attribution check above makes.
. "$ROOT/tests/conformance/tap-verdict.sh"

tap_verdict "$ROOT/build/amiberry-testhd-$TAG/bsdsocktest.log" "$status"
exit $?
