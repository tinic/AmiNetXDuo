#!/bin/bash
#
# One emulator run, gated at both ends.
#
#   emurun.sh -b BUILD -m MODEL -T TAG -P PORT [-B BYTES] [-w WORKLOAD] [-p]
#
# Output is key=value on stdout and one RESULT= line last.  Exit codes:
#   0 ok   1 the run failed   2 refused before starting   3 ran, no verdict
#
set -u

BUILD=""; MODEL="A1200"; TAG=""; PORT=""; BYTES=524288; WORKLOAD=bench; PROF=0
STACK=ours; BORROW_UG=0; EXTRALIBS=""
while getopts "b:m:T:P:B:w:s:L:pU" o; do
    case "$o" in
        b) BUILD="$OPTARG" ;; m) MODEL="$OPTARG" ;; T) TAG="$OPTARG" ;;
        P) PORT="$OPTARG" ;;  B) BYTES="$OPTARG" ;; w) WORKLOAD="$OPTARG" ;;
        s) STACK="$OPTARG" ;;
        U) BORROW_UG=1 ;;
        L) EXTRALIBS="$OPTARG" ;;
        p) PROF=1 ;;
        *) echo "RESULT=usage"; exit 2 ;;
    esac
done
[ -n "$TAG" ] && [ -n "$PORT" ] || {
    echo "reason=missing_args"; echo "RESULT=refused"; exit 2; }
# -b names OUR build; a foreign stack is located by run-compare.sh instead.
if [ "$STACK" = ours ] && [ -z "$BUILD" ]; then
    echo "reason=missing_build_for_ours"; echo "RESULT=refused"; exit 2
fi

cd ~/anxd-e2e || { echo "reason=no_checkout"; echo "RESULT=refused"; exit 2; }
. ~/amiga-assets/env.sh 2>/dev/null
. tools/amiga-toolchain.sh >/dev/null 2>&1

fail() { echo "reason=$1"; echo "RESULT=refused"; exit 2; }

# ---- preflight -----------------------------------------------------------

if [ "$STACK" = ours ]; then
[ -d "$BUILD" ] || fail "no_build_dir:$BUILD"
for f in src/bsdsocket/bsdsocket.library src/tools/NetTrace src/tools/AddNetInterface; do
    [ -f "$BUILD/$f" ] || fail "build_missing:$f"
done
[ "$PROF" = 0 ] || [ -f "$BUILD/tools/profiler/Profile" ] || fail "build_missing:Profile"
fi

# The ROM for this model, by the same rule amiberry-run.sh uses.
_ks="AMINETXDUO_KICKSTART_$(printf '%s' "$MODEL" | tr '[:lower:]' '[:upper:]' | tr -c 'A-Z0-9' '_')"
_ks="${_ks%_}"
[ -n "${!_ks:-${AMINETXDUO_KICKSTART:-}}" ] || fail "no_kickstart_for:$MODEL"
[ -n "${AMINETXDUO_A2065:-}" ] || [ -f build/a2065.device ] || fail "no_a2065_device"

# The build's target CPU against the machine.  A 68020 object stops a 68000
# with an illegal instruction before anything under test runs.
#
# From CMakeCache, NOT from objdump: `objdump -f` on a hunk binary reports
# "m68k:68000" for a tree compiled -m68020, so the obvious check passes a
# 68020 build onto an A600 and the gate is worse than none.  The cache and
# flags.make agree with each other and with the compiler.
# The library says so itself now, which is the only thing that works on a
# binary out of a release archive rather than a build tree.
_arch=$(strings -a "$BUILD/src/bsdsocket/bsdsocket.library" 2>/dev/null |
        sed -n 's/.*\$VER: bsdsocket\.library .* \([0-9]\{5\}\|any\)$/\1/p' | head -1)
[ -n "$_arch" ] ||
_arch=$(sed -n 's/^AMINETXDUO_CPU:[^=]*=//p' "$BUILD/CMakeCache.txt" 2>/dev/null)
[ -n "$_arch" ] || _arch=$(grep -m1 -oE '\-m68[0-9]+' \
    "$BUILD/src/bsdsocket/CMakeFiles/bsdsocket_library.dir/flags.make" 2>/dev/null |
    tr -d '\-m')
# Only meaningful for our own build; a foreign stack's binaries are whatever
# its vendor shipped and we do not get to check them.
if [ "$STACK" = ours ]; then
    [ -n "$_arch" ] || fail "cpu_unknown:$BUILD"
fi
# `any` passes every model: it is -m68000 codegen with the inner loops chosen
# from AttnFlags, so the A600 arm is exactly what it is for.
if [ "$STACK" = ours ]; then
case "$MODEL" in
    A500|A500+|A600) [ "$_arch" = "68000" ] || [ "$_arch" = "any" ] ||
                     fail "cpu_mismatch:${_arch}_on_$MODEL" ;;
esac
fi

# A stale emulator holds the emulated network and poisons the next run.
# pgrep -c prints 0 AND exits 1 when nothing matches, so `|| echo 0` appends a
# second 0 and the comparison never holds.  That made this gate refuse every
# run, which is how a gate fails safe and useless at the same time.
#
# Reported, never killed: this host carries other checkouts, and a bare
# `pkill -f amiberry` takes their runs down with it.  The config path is
# printed so the caller can see whose it is.
# Scoped to THIS checkout: the host carries other agents' trees and one of
# their runs is not our problem to refuse over, nor ours to kill.
_mine=$(pgrep -af '[a]miberry' 2>/dev/null | grep -c "$PWD" || true)
[ -n "$_mine" ] || _mine=0
if [ "$_mine" -ne 0 ] 2>/dev/null; then
    pgrep -af '[a]miberry' 2>/dev/null | grep "$PWD" | sed 's/^/stale_proc=/' | head -4
    fail "stale_emulator:$_mine"
fi

# A defunct emulator has released nothing its parent has not reaped, and it
# does not answer pgrep's live count the same way.  Its sockets can still be
# bound, which is the state that produces a run with no network and no reason.
_zomb=$(ps -eo stat=,comm= 2>/dev/null | awk '$1 ~ /^Z/ && $2 ~ /amiberry/' | wc -l)
# A defunct emulator belongs to whoever spawned it; it is reported, not killed.
[ "${_zomb:-0}" -eq 0 ] 2>/dev/null || fail "zombie_emulator:$_zomb"

# The peer helpers outlive a killed run and hold the port the next one wants.
for _p in httppeer fitz-serve; do
    _n=$(pgrep -c "[${_p:0:1}]${_p:1}" 2>/dev/null)
    [ -n "$_n" ] || _n=0
    [ "$_n" -eq 0 ] 2>/dev/null || fail "stale_peer:$_p:$_n"
done

# The peer port has to be ours alone; two runs sharing one was a measured
# artefact once already.  ANY state, not just LISTEN: a socket left in
# TIME_WAIT or still ESTABLISHED from a killed run refuses the bind just as
# effectively and shows up in neither `ss -ltn` nor a process list.
if command -v ss >/dev/null 2>&1; then
    _busy=$(ss -tan 2>/dev/null | awk -v p=":$PORT" '$4 ~ p"$" || $5 ~ p"$"' | wc -l)
    [ "${_busy:-0}" -eq 0 ] 2>/dev/null || {
        ss -tan 2>/dev/null | awk -v p=":$PORT" '$4 ~ p"$" || $5 ~ p"$"' |
            sed 's/^/port_state=/' | head -3
        fail "port_in_use:$PORT:$_busy"
    }
fi

echo "stack=$STACK"
echo "model=$MODEL"
echo "build=$BUILD"
echo "arch=$_arch"
echo "bytes=$BYTES"
echo "tag=$TAG"

# ---- run -----------------------------------------------------------------

RUNNER=tests/compare/run-compare.sh
[ -x "$RUNNER" ] || fail "no_runner:$RUNNER"

# -p pointed at tests/compare/run-compare-prof.sh, which has never existed in
# this repository's history: the option was written against a script that was
# never committed, so every -p run has always died on the `[ -x ]` above.  The
# profiling comparison that does exist is tests/perf/run-stackprof.sh.  Said
# here rather than left as a path that fails to resolve, because a missing
# file reads as a tree that has lost something.
[ "$PROF" = 0 ] || fail "prof_runner_never_existed:see the note above"

# A watchdog on PROGRESS, not on total time.
#
# The guest writes stdout.txt as it works.  A run whose guest has written
# nothing by the deadline is not slow, it is dead: the card is up, the machine
# is at 50 fps, and nothing is coming.  Waiting out the 900 s timeout to learn
# that costs fifteen minutes and produces the same empty file.  Roadshow leases
# on the slowest machine here in 24 s.
#
# AMINETXDUO_EMURUN_PROGRESS overrides; 120 s is four times the slowest known
# bring-up.
PROGRESS_S="${AMINETXDUO_EMURUN_PROGRESS:-120}"
HD="build/amiberry-testhd-$TAG"

# A foreign stack that ships no usergroup.library needs ours, or every
# command that resolves a user or a service fails before it starts.
_bopt=(); [ -z "$BUILD" ] || _bopt=(-b "$BUILD")
[ "$BORROW_UG" = 0 ] || _bopt+=(-U)
# A stack may need libraries the bare boot shell has no LIBS: for -- rexxsyslib
# and the math ones are the usual pair.
[ -z "$EXTRALIBS" ] || _bopt+=(-L "$EXTRALIBS")
"$RUNNER" -s "$STACK" -w "$WORKLOAD" "${_bopt[@]}" -m "$MODEL" -T "$TAG" \
          -P "$PORT" -B "$BYTES" -t 900 > "/tmp/emurun-$TAG.log" 2>&1 &
_runner_pid=$!

(
    _waited=0
    while kill -0 "$_runner_pid" 2>/dev/null; do
        if [ -s "$HD/stdout.txt" ]; then
            exit 0                      # the guest is alive; let it finish
        fi
        [ "$_waited" -ge "$PROGRESS_S" ] && break
        sleep 5
        _waited=$((_waited + 5))
    done
    kill -0 "$_runner_pid" 2>/dev/null || exit 0
    echo "reason=no_guest_progress_in_${PROGRESS_S}s" >&2
    pkill -9 -f "amiberry-$TAG[.]uae" 2>/dev/null
    kill -9 "$_runner_pid" 2>/dev/null
) &
_watchdog=$!

wait "$_runner_pid" 2>/dev/null
rc=$?
kill "$_watchdog" 2>/dev/null
echo "runner_rc=$rc"

# ---- postflight ----------------------------------------------------------
#
# A run that reports success having produced nothing is the failure this file
# exists for.  The artifacts decide, not the exit code.

guest=$( [ -s "$HD/stdout.txt" ] && echo yes || echo no )
echo "guest_output=$guest"

wire=$(grep -acE "^wire: " "/tmp/emurun-$TAG.log" 2>/dev/null || echo 0)
loop=$(grep -acE "^loopback: " "/tmp/emurun-$TAG.log" 2>/dev/null || echo 0)
echo "wire_lines=$wire"
echo "loopback_lines=$loop"

if [ "$PROF" = 1 ]; then
    prof=$( [ -s "$HD/wire.prof" ] && echo yes || echo no )
    echo "profile=$prof"
fi

grep -aE "^(wire|loopback): " "/tmp/emurun-$TAG.log" 2>/dev/null | sed 's/^/measure=/'

if [ "$guest" = no ]; then
    echo "RESULT=incomplete"; exit 3
fi
if [ "$wire" = "0" ] && [ "$loop" = "0" ]; then
    echo "RESULT=incomplete"; exit 3
fi
if [ "$PROF" = 1 ] && [ ! -s "$HD/wire.prof" ]; then
    echo "RESULT=incomplete"; exit 3
fi
[ "$rc" = "0" ] || { echo "RESULT=fail"; exit 1; }
echo "RESULT=ok"
exit 0
