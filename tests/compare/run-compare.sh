#!/usr/bin/env bash
#
# Run the same workload against a DIFFERENT bsdsocket.library.
#
#   tests/compare/run-compare.sh -s STACK -w WORKLOAD [options]
#
# WHY THIS EXISTS
#
#   Every other harness in this tree stages OUR library out of a build
#   directory, which makes it structurally incapable of measuring anybody
#   else's.  The comparison that is worth having is against the stacks this
#   project is compatible with -- Roadshow, whose ABI we implement, and
#   AmiTCP_NG, which implements the same one -- on the same emulator profile,
#   the same ROM, the same a2065.device and the same host, back to back.
#
#   So the stack is a parameter here, and everything else is held fixed:
#
#     * the driver is tests/compare/checkrunner.c, built once and reused,
#       which runs each command with a 512 KB stack and records its exit
#       code and elapsed ticks -- so the DHCP figure is the stack's own
#       AddNetInterface and the throughput figure is one binary run twice;
#     * NetTrace comes out of ONE build and is staged unchanged against every
#       stack.  It links nothing of src/: every call into the library is a
#       published LVO through toolsock.c's inline `jsr a6@(-n:W)`, so it is as
#       foreign to our stack as to theirs;
#     * bsdsocktest is the upstream suite, which knows about none of us.
#
# NOTHING OF THEIRS IS COPIED INTO THIS REPOSITORY.  Both foreign stacks are
# located at run time through a path -- AMINETXDUO_CMP_ROADSHOW and
# AMINETXDUO_CMP_AMITCPNG, or -R / -G -- and staged straight from wherever
# they were unpacked.  Roadshow is a commercial demo and AmiTCP_NG is GPL;
# this tree is MIT and stays that way.
#
# OPTIONS
#
#   -s ours|roadshow|amitcpng     which stack (required)
#   -w bench|conf|diag            which workload (required)
#        bench  AddNetInterface (= time to a DHCP lease), ping, and NetTrace
#               on loopback and over the wire
#        conf   the bsdsocktest conformance suite, tier chosen with -a
#        diag   bring-up only, plus whatever -X adds
#   -a "ARGS"   bsdsocktest's own argument line (default "NOPAGE";
#               "LOOPBACK NOPAGE" for the loopback tier).  A line containing
#               HOST starts the suite's host helper on this machine.
#   -b DIR      build directory for the `ours` stack (default build/cm)
#   -B BYTES    NetTrace workload size (default 524288, as docs/RESEARCH.md 24)
#   -m MODEL    emulator profile (default A1200 -- the only timing profile)
#   -t SECS     timeout
#   -T TAG      run tag; results land in build/testhd-<tag>/
#   -P PORT     base port for the workload peer (default 7500)
#   -R DIR      Roadshow Workbench/ directory
#   -G DIR      AmiTCP_NG data/ directory (the one holding Libs/ and C/)
#   -i FILE     use FILE as DEVS:NetInterfaces/eth0 instead of the tree's
#   -C DIR      stage every command in DIR at DH0:
#   -L DIR      stage every file in DIR into LIBS:
#   -X FILE     extra plan lines (NAME<TAB>COMMAND), appended
#   -U          stage OUR usergroup.library when the stack has none of its own
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)

STACK=""
WORKLOAD=""
CONF_ARGS="NOPAGE"
BUILD="${AMINETXDUO_BUILD:-build/cm}"
NT_BYTES=524288
MODEL=A1200
TIMEOUT=900
TAG=""
BASE_PORT=7500
RSDIR="${AMINETXDUO_CMP_ROADSHOW:-/tmp/rsdemo/Roadshow-Demo-1.15/Workbench}"
NGDIR="${AMINETXDUO_CMP_AMITCPNG:-}"
BORROW_UG=0
IFCONFIG=""
EXTRA_PLAN=""
CMDDIR=""
EXTRALIBS=""
STAGED_CMDS=()

while getopts "s:w:a:b:B:m:t:T:P:R:G:i:X:C:L:U" opt; do
    case "$opt" in
        s) STACK="$OPTARG" ;;
        w) WORKLOAD="$OPTARG" ;;
        a) CONF_ARGS="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        B) NT_BYTES="$OPTARG" ;;
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        T) TAG="$OPTARG" ;;
        P) BASE_PORT="$OPTARG" ;;
        R) RSDIR="$OPTARG" ;;
        G) NGDIR="$OPTARG" ;;
        i) IFCONFIG="$OPTARG" ;;
        X) EXTRA_PLAN="$OPTARG" ;;
        C) CMDDIR="$OPTARG" ;;
        L) EXTRALIBS="$OPTARG" ;;
        U) BORROW_UG=1 ;;
        *) sed -n '3,60p' "$0" >&2; exit 2 ;;
    esac
done

[ -n "$STACK" ] && [ -n "$WORKLOAD" ] || { sed -n '3,60p' "$0" >&2; exit 2; }
[ -n "$TAG" ] || TAG="cmp-$STACK-$WORKLOAD"

# -b may be relative to the tree or an absolute path elsewhere.  The second is
# not a convenience: build/ directory names are shared, and docs/RESEARCH.md
# 24.9 records two measurements lost to another workstream rebuilding the
# instrument underneath them.  A controlled arm points at a build directory
# nothing else writes to.
case "$BUILD" in
    /*) ;;
    *)  BUILD="$ROOT/$BUILD" ;;
esac

# --------------------------------------------------------------- the stack --
#
# Three things are wanted from each: the library, an AddNetInterface, and a
# ping.  Every one of them is that stack's own, because a stack that needed
# somebody else's command to start would not be a stack under test.

LIBBSD=""
LIBUG=""
CMD_ADDIF=""
CMD_PING=""
EXTRA_STAGE=()
STACK_NOTE=""

case "$STACK" in
    ours)
        LIBBSD="$BUILD/src/bsdsocket/bsdsocket.library"
        LIBUG="$BUILD/src/usergroup/usergroup.library"
        CMD_ADDIF="$BUILD/src/tools/AddNetInterface"
        CMD_PING="$BUILD/src/tools/ping"
        STACK_NOTE="AmiNetXDuo from $BUILD"
        ;;
    roadshow)
        [ -d "$RSDIR" ] || { echo "no Roadshow at $RSDIR (-R DIR)" >&2; exit 2; }
        LIBBSD="$RSDIR/Libs/bsdsocket.library"
        LIBUG="$RSDIR/Libs/usergroup.library"
        CMD_ADDIF="$RSDIR/C/AddNetInterface"
        CMD_PING="$RSDIR/C/ping"
        STACK_NOTE="Roadshow from $RSDIR"
        ;;
    amitcpng)
        [ -n "$NGDIR" ] && [ -d "$NGDIR" ] || {
            echo "no AmiTCP_NG: pass -G <dir>/AmiTCP_NG/data or set" \
                 "AMINETXDUO_CMP_AMITCPNG" >&2; exit 2; }
        LIBBSD="$NGDIR/Libs/bsdsocket.library"
        LIBUG="$NGDIR/Libs/usergroup.library"
        CMD_ADDIF="$NGDIR/C/AddNetInterface"
        CMD_PING="$NGDIR/C/ping"
        # Its library reads a configuration through an AmiTCP: assign and
        # falls back to SYS:AmiTCP, which a directory hard drive can supply
        # without a C:Assign the bare boot shell does not have.
        [ -d "$NGDIR/db" ] && EXTRA_STAGE+=("$NGDIR/db")
        STACK_NOTE="AmiTCP_NG from $NGDIR"
        ;;
    *)  echo "unknown stack '$STACK'" >&2; exit 2 ;;
esac

[ -f "$LIBBSD" ] || { echo "missing $LIBBSD" >&2; exit 2; }
for f in "$CMD_ADDIF" "$CMD_PING"; do
    [ -f "$f" ] || echo "!! $STACK has no $(basename "$f")" >&2
done

# ------------------------------------------------------------- ingredients --

DRIVER="$ROOT/build/compare/CheckRunner"
NETTRACE="${AMINETXDUO_CMP_NETTRACE:-$BUILD/src/tools/NetTrace}"
SUITE="$ROOT/build/bsdsocktest/bsdsocktest"

. "$ROOT/tools/amiga-toolchain.sh"
mkdir -p "$ROOT/build/compare"
if [ ! -x "$DRIVER" ] || [ "$ROOT/tests/compare/checkrunner.c" -nt "$DRIVER" ]; then
    echo "==> building CheckRunner"
    "$AMIGA_GCC" -O2 -m68020 -fomit-frame-pointer -Wall -I"$AMIGA_NDK" \
                 -o "$DRIVER" "$ROOT/tests/compare/checkrunner.c"
fi

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

# --------------------------------------------------------------- staging ----

STAGE="$ROOT/build/cmp-stage-$TAG"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs" "$STAGE/w" "$STAGE/d"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"

# -i replaces DEVS:NetInterfaces/eth0 with a file of the caller's choosing.
# It exists because "the interface would not come up" has two causes that look
# identical from the outside -- the stack cannot drive the card, or it did not
# understand the configuration file -- and a stack should be measured with a
# configuration it agrees is well formed.
if [ -n "${IFCONFIG:-}" ]; then
    [ -f "$IFCONFIG" ] || { echo "no such interface file: $IFCONFIG" >&2; exit 2; }
    cp "$IFCONFIG" "$STAGE/devs/NetInterfaces/eth0"
    echo "==> interface configuration: $IFCONFIG"
fi

# The device goes in BOTH places on purpose: ours opens it out of DEVS:, and
# both foreign stacks look in DEVS:Networks/.  One copy of a driver in two
# directories removes a variable that would otherwise look like a bug in
# somebody's SANA-II handling.
cp "$A2065" "$STAGE/devs/a2065.device"
mkdir -p "$STAGE/devs/Networks"
cp "$A2065" "$STAGE/devs/Networks/a2065.device"

cp "$LIBBSD" "$STAGE/libs/bsdsocket.library"
if [ -f "$LIBUG" ]; then
    cp "$LIBUG" "$STAGE/libs/usergroup.library"
elif [ "$BORROW_UG" = "1" ]; then
    OURUG="$BUILD/src/usergroup/usergroup.library"
    [ -f "$OURUG" ] && { cp "$OURUG" "$STAGE/libs/usergroup.library"
                         echo "==> $STACK ships no usergroup.library: staging OURS" ; }
else
    echo "==> $STACK ships no usergroup.library and none was borrowed (-U)"
fi

# Roadshow's ping is built against a C library that opens mathieeedoubbas
# before main(), and a bare directory hard drive has no LIBS: to find it in.
# Staging it for every stack costs nothing and is not a thumb on the scale:
# it is a maths library, not a network one.
# -L stages a whole LIBS: tree of the caller's own -- amisslmaster.library and
# its versioned library under AmiSSL/, which no stack here supplies.
# Defaulted, because the failure without it is unreadable: a client that opens
# a library BEFORE main() gives no output at all when it is missing, only a
# return code -- indistinguishable from the stack under test being broken.
# That cost an hour once, on the third-party curl this harness no longer runs;
# the trap is the pre-main open, not that client, so the default stays.
# build/amissl-stage/libs is where the TLS tests leave the tree.
if [ -z "${EXTRALIBS:-}" ] && [ -d "$ROOT/build/amissl-stage/libs" ]; then
    EXTRALIBS="$ROOT/build/amissl-stage/libs"
    # echo, NOT say(): say() is defined ~40 lines below this and appends to
    # $PLAN, so calling it here was "command not found" under set -e -- rc 127
    # for every run that had build/amissl-stage/libs, which is every run after
    # the TLS tests have been built once. It was committed as a cosmetic change
    # to where the message goes; it was not, it was a fatal one.
    echo "==> staging $EXTRALIBS into LIBS: (default; -L overrides)"
fi

if [ -n "${EXTRALIBS:-}" ]; then
    cp -R "$EXTRALIBS"/* "$STAGE/libs/"
fi

# build/amissl-mathlibs first, and it has to be: the two have to be a matched
# pair or a clib2 client sits there forever, and tools/amissl-run.sh says why.
# Loose in build/ they are not a pair -- the doubtrans there is the -os one and
# the doubbas is not -- so taking them from a directory that holds both is the
# difference between a client that runs and a run that times out.
for lib in mathieeedoubbas mathieeedoubtrans; do
    for cand in "$ROOT/build/amissl-mathlibs/$lib.library" \
                "$ROOT/build/$lib-os.library" "$ROOT/build/$lib.library"; do
        [ -f "$cand" ] && { cp "$cand" "$STAGE/libs/$lib.library"; break; }
    done
done

# -c DIR stages every command in a directory at DH0:, which is what a plan
# line addressed as DH0:<name> then runs.  Diagnosis needs it: when a stack
# will not come up, its own status commands are the only thing that can say
# why, and they are not the two this harness knows about by name.
if [ -n "${CMDDIR:-}" ]; then
    for f in "$CMDDIR"/*; do
        [ -f "$f" ] || continue
        cp "$f" "$STAGE/$(basename "$f")"
        STAGED_CMDS+=("$STAGE/$(basename "$f")")
    done
fi

[ -f "$CMD_ADDIF" ] && cp "$CMD_ADDIF" "$STAGE/AddNetInterface"
[ -f "$CMD_PING" ]  && cp "$CMD_PING"  "$STAGE/ping"

# AmiTCP_NG's configuration, staged where SYS:AmiTCP finds it.
for extra in "${EXTRA_STAGE[@]}"; do
    mkdir -p "$STAGE/AmiTCP"
    cp -R "$extra" "$STAGE/AmiTCP/"
done

STAGED=("$STAGE/devs" "$STAGE/libs" "$STAGE/w" "$STAGE/d")
[ -f "$STAGE/AddNetInterface" ] && STAGED+=("$STAGE/AddNetInterface")
[ -f "$STAGE/ping" ] && STAGED+=("$STAGE/ping")
[ -d "$STAGE/AmiTCP" ] && STAGED+=("$STAGE/AmiTCP")

PLAN="$STAGE/checks.txt"
: > "$PLAN"
say() { printf '%s\n' "$*" >> "$PLAN"; }

# Every workload starts the network the same way, with the stack's own
# command, and the driver times it.  That elapsed time IS the DHCP figure:
# all three commands block until the interface has an address or the attempt
# has failed.
say "# generated by tests/compare/run-compare.sh -s $STACK -w $WORKLOAD"
if [ -f "$STAGE/AddNetInterface" ]; then
    printf 'addif\tDH0:AddNetInterface DEVS:NetInterfaces/eth0\n' >> "$PLAN"
fi

PEER_KIND=""

case "$WORKLOAD" in
    bench)
        [ -f "$NETTRACE" ] || { echo "missing $NETTRACE" >&2; exit 2; }
        cp "$NETTRACE" "$STAGE/NetTrace"
        STAGED+=("$STAGE/NetTrace")
        PEER_KIND="httppeer"
        printf 'loopwarm\tDH0:NetTrace LOOPBACK NOCAPTURE BYTES=%s\n' \
               "$NT_BYTES" >> "$PLAN"
        printf 'loop\tDH0:NetTrace LOOPBACK NOCAPTURE BYTES=%s\n' \
               "$NT_BYTES" >> "$PLAN"
        printf 'wirewarm\tDH0:NetTrace WIRE HOST=10.0.2.2 PORT=%s PATH=/bytes/%s BYTES=%s NOCAPTURE\n' \
               "$BASE_PORT" "$NT_BYTES" "$NT_BYTES" >> "$PLAN"
        printf 'wire\tDH0:NetTrace WIRE HOST=10.0.2.2 PORT=%s PATH=/bytes/%s BYTES=%s NOCAPTURE\n' \
               "$BASE_PORT" "$NT_BYTES" "$NT_BYTES" >> "$PLAN"
        # ping LAST: one Roadshow boot hung immediately after it and took
        # that boot's throughput figures with it.  A step that can end the run
        # goes after the steps that cannot.
        [ -f "$STAGE/ping" ] &&
            printf 'ping\tDH0:ping 10.0.2.2 -c 5 -t 15\n' >> "$PLAN"
        ;;
    conf)
        [ -f "$SUITE" ] || {
            echo "missing $SUITE -- run tests/conformance/build.sh" >&2; exit 2; }
        cp "$SUITE" "$STAGE/bsdsocktest"
        STAGED+=("$STAGE/bsdsocktest")
        case "$CONF_ARGS" in *HOST*) PEER_KIND="helper" ;; esac
        printf 'conf\tDH0:bsdsocktest %s\n' "$CONF_ARGS" >> "$PLAN"
        ;;
    diag)
        # Bring-up and whatever -X asks for, and nothing else.  A stack that
        # will not come up is diagnosed here rather than inside a workload
        # whose failure mode is a four-hundred-second timeout.
        ;;
    *)  echo "unknown workload '$WORKLOAD'" >&2; exit 2 ;;
esac

# -X appends plan lines of the caller's own, which is how a stack that will
# not come up gets diagnosed without a new harness: NAME<TAB>COMMAND, run in
# order after everything above.
if [ -n "${EXTRA_PLAN:-}" ]; then
    [ -f "$EXTRA_PLAN" ] || { echo "no such plan file: $EXTRA_PLAN" >&2; exit 2; }
    cat "$EXTRA_PLAN" >> "$PLAN"
fi

STAGED+=("$PLAN")
[ ${#STAGED_CMDS[@]} -eq 0 ] || STAGED+=("${STAGED_CMDS[@]}")

# ------------------------------------------------------------------ peers ---

PEER_PID=""
cleanup() { [ -z "$PEER_PID" ] || kill -TERM "$PEER_PID" 2>/dev/null || true; }
trap cleanup EXIT INT TERM HUP

case "$PEER_KIND" in
    httppeer)
        python3 "$ROOT/tests/peer/httppeer.py" --base-port "$BASE_PORT" \
                --log "$ROOT/build/cmppeer-$TAG.log" \
                --seconds "$((TIMEOUT + 300))" \
                > "$ROOT/build/cmppeer-$TAG.out" 2>&1 &
        PEER_PID=$!
        ;;
    helper)
        # The suite derives every helper port from a fixed 8700, with no way
        # to move it, so two workstreams cannot each have their own.  One
        # already listening is therefore used rather than fought over: it is
        # stateless per connection, the emulator lock means only one guest is
        # ever talking to it, and a second bind would just fail.
        if lsof -nP -iTCP:8700 -sTCP:LISTEN >/dev/null 2>&1; then
            echo "==> a bsdsocktest helper is already listening on 8700; using it"
        else
            python3 "$ROOT/third_party/bsdsocktest/host/bsdsocktest_helper.py" \
                    > "$ROOT/build/cmphelper-$TAG.out" 2>&1 &
            PEER_PID=$!
        fi
        ;;
esac

if [ -n "$PEER_PID" ]; then
    sleep 2
    kill -0 "$PEER_PID" 2>/dev/null || {
        echo "the host peer did not start:" >&2
        cat "$ROOT/build/cmp"*"-$TAG.out" >&2; exit 2; }
fi

# -------------------------------------------------------------------- run ---

export AMINETXDUO_RUN_TAG="$TAG"

# FS-UAE's own bsdsocket.library emulation would otherwise be a fourth stack
# in the room.  It cannot be turned off from the configuration
# tools/fsuae-run.sh writes, so it goes into the run's private base directory,
# exactly as tests/conformance/run-fsuae.sh does it.
BASEDIR="$ROOT/build/fsuae-base-$TAG"
mkdir -p "$BASEDIR/Configurations"
cat > "$BASEDIR/Configurations/Host.fs-uae" <<'EOF'
[fs-uae]
floppy_drive_volume = 0
floppy_drive_volume_empty = 0
bsdsocket_library = 0
EOF

echo "==> stack:    $STACK  ($STACK_NOTE)"
echo "==> workload: $WORKLOAD on $MODEL, peer 127.0.0.1:$BASE_PORT"
echo "==> plan:"
sed 's/^/      /' "$PLAN"

set +e
"$ROOT/tools/fsuae-run.sh" -n -m "$MODEL" -t "$TIMEOUT" \
    "$DRIVER" "${STAGED[@]}" > "$ROOT/build/cmp-$TAG.log" 2>&1
RC=$?
set -e

HD="$ROOT/build/testhd-$TAG"

echo
echo "================ $STACK / $WORKLOAD ================"
[ -f "$HD/results.txt" ] && { echo "-- results (name rc ticks availmem)"
                              cat "$HD/results.txt"; }
for f in "$HD"/w/*.txt; do
    [ -f "$f" ] || continue
    echo "-- $(basename "$f")"
    tr -d '\r' < "$f" | head -60
done
[ -f "$HD/bsdsocktest.log" ] && {
    echo "-- bsdsocktest summary"
    grep -E "^# (bsdsocket|Passed|Failed|Skipped|Total)|^1\.\.|^# Roadshow" \
        "$HD/bsdsocktest.log" | head -20
    # TAP counts a skip as `ok N ... # SKIP`, so the passed figure is the ok
    # lines LESS the skips.  Reporting the raw ok count would have called our
    # loopback tier 142/142 when the suite's own summary says 130 passed, 12
    # skipped -- the difference between a headline and a wrong headline.
    ok=$(grep -c "^ok " "$HD/bsdsocktest.log" || true)
    sk=$(grep -c "# SKIP" "$HD/bsdsocktest.log" || true)
    nk=$(grep -c "^not ok " "$HD/bsdsocktest.log" || true)
    echo "   passed:  $((ok - sk))"
    echo "   failed:  $nk"
    echo "   skipped: $sk"
    grep "^not ok " "$HD/bsdsocktest.log" | head -20
}

echo
echo "emulator log: build/cmp-$TAG.log"
echo "guest files:  $HD"
exit "$RC"
