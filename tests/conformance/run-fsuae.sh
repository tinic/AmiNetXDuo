#!/usr/bin/env bash
#
# Run the bsdsocktest conformance suite against our bsdsocket.library.
#
#   tests/conformance/run-fsuae.sh [-m MODEL] [-c CPU] [-t SECONDS]
#                                  [-T TAG] [-a "SUITE ARGS"] [-p] [-b BUILDDIR]
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
# FS-UAE ships its own bsdsocket.library emulation.  If it answered
# OpenLibrary() the whole run would be measuring WinUAE's host-socket shim
# instead of our stack, so this script sets bsdsocket_library = 0 via a host
# configuration dropped into the run's private base directory (FS-UAE reads
# it after resolving base_dir; the config tools/amiberry-run.sh writes cannot
# carry the option).
#
# The emulator log still says "bsdsocket.library installed" either way, that
# line is printed when the emulation is *built*, not when it is registered.
# It is not the proof.  The proof is threefold and checked below / visible in
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
#   build/testhd-<tag>/bsdsocktest.log   the TAP log, the actual result
#   build/testhd-<tag>/conf-out.txt      the suite's console summary
#   build/serial-<tag>.log               our ami_log output
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
# AMINETXDUO_RUN_TAG values would silently share build/testhd-conformance and
# clobber each other's results.
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

# Turn off FS-UAE's own bsdsocket.library emulation.  fsuae-run.sh only
# mkdir -p's the base directory, so a file placed here survives.
BASEDIR="$ROOT/build/fsuae-base-$TAG"
mkdir -p "$BASEDIR/Configurations"
cat > "$BASEDIR/Configurations/Host.fs-uae" <<'EOF'
[fs-uae]
floppy_drive_volume = 0
floppy_drive_volume_empty = 0
bsdsocket_library = 0
EOF

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

echo "---- stack under test ----"
ident=$(grep -m1 "^# bsdsocket.library:" \
        "$ROOT/build/amiberry-testhd-$TAG/bsdsocktest.log" 2>/dev/null || true)
case "$ident" in
    *AmiNetXDuo*) echo "$ident  (ours)" ;;
    "")           echo "!! no stack identification in the TAP log" ;;
    *)            echo "!! $ident, NOT our library, results are meaningless" ;;
esac

exit "$status"
