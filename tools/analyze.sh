#!/usr/bin/env bash
#
# Run GCC's -fanalyzer over our own sources and compare the result with
# tools/analyzer-baseline.txt.
#
#   tools/analyze.sh            # check: new findings are a failure
#   tools/analyze.sh --update   # rewrite the baseline from this run
#
# WHY A BASELINE AND NOT -Werror
#
#   The tree is not analyser-clean and cannot be made so by editing it.  What
#   is left after triage is 25 findings in five groups, none of them defects:
#
#     * six in src/bsdsocket/netstatus.c, where the buffer-has-room invariant
#       goes through an integer division the analyser's constraint solver does
#       not follow.  The comment above the switch in bsd_NetStackQuery() has
#       the proof.
#     * twelve in tests/tcpdrill/tcpdrill.c, where say() is a hand-rolled
#       printf.  The analyser does not read the format string through the call,
#       so it walks every conversion branch and reports the ones that do not
#       match the arguments actually passed.  Every real pair matches.
#     * one in port/threadx-amiga/src/tx_thread_stack_build.c, where Wait(0UL)
#       is a deliberate never-returns trap and the analyser continues past it.
#     * one in tests/tools/ifprobe.c, where p_poison() fills the struct in a
#       loop the analyser unrolls once.
#     * five uninitialised E-Clock reads in the units listed as falling back
#       below -- the NDK blind spot this script otherwise removes.
#
#   Silencing those would mean adding dead NULL tests, pragmas over whole
#   functions, or a rewrite of a test's formatter -- all of which cost more
#   than they buy.  So they are recorded, and anything NEW fails the build.
#   Verified to do that: a `return *(long *)0` planted in src/sana2 is reported
#   as a new finding and exits 1.
#
#   The baseline is keyed on file + warning + message text, with a count.  Not
#   on line numbers: a baseline that moves when an unrelated line above it
#   moves gets regenerated blindly, and then it is not a gate.
#
# WHY -D_NO_INLINE
#
#   The NDK spells every system call as an inline asm statement expression --
#   inline/macros.h LPn(), a `jsr a6@(-offs:W)` with a "memory" clobber and no
#   output constraint for the destination.  -fanalyzer does not read a "memory"
#   clobber as a store, so after ReadEClock(&ev) it still believes ev.ev_lo is
#   uninitialised.  Measured: 32 of 48 findings on this tree were that, and
#   only that -- the same call declared as a plain extern produces none.
#
#   <proto/*.h> already has the switch: _NO_INLINE takes the clib/*_protos.h
#   prototypes instead of the inline macros.  Objects built that way would not
#   link, which is why this pass compiles to /dev/null and the real build is
#   left alone.  A handful of files do not compile under it -- they hand-roll
#   an LPnNR() for a call the NDK has no prototype for -- and those fall back
#   to a normal -fanalyzer pass so they are still covered, just noisier.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"

BASELINE="tools/analyzer-baseline.txt"
BUILD="${AMINETXDUO_ANALYZE_BUILD:-build/analyze}"
JOBS="${AMINETXDUO_CI_JOBS:-$( (command -v nproc >/dev/null && nproc) || sysctl -n hw.ncpu 2>/dev/null || echo 4 )}"
UPDATE=0

for arg in "$@"; do
    case "$arg" in
        --update) UPDATE=1 ;;
        *) echo "usage: tools/analyze.sh [--update]" >&2; exit 2 ;;
    esac
done

if [ -z "${AMIGA_TOOLCHAIN_ROOT:-}" ]; then
    . tools/amiga-toolchain.sh
fi

# ------------------------------------------------------- the compile lines ---
#
# Configure only: compile_commands.json is written at generate time, and this
# pass never wants the objects.

cmake -S . -B "$BUILD" \
    -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-m68k-amigaos.cmake \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DCMAKE_C_FLAGS=-fanalyzer > "$BUILD-configure.log" 2>&1 || {
        tail -30 "$BUILD-configure.log"; exit 1; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# compile_commands.json, one command per line, ours only, deduplicated.  CMake
# writes no backslash escapes into it for this project (the whole file is
# checked below), so sed is enough and this script needs no JSON parser.
if grep -q '\\' "$BUILD/compile_commands.json"; then
    echo "compile_commands.json contains escapes -- this parser is too naive" >&2
    exit 1
fi
grep '"command":' "$BUILD/compile_commands.json" |
    sed -e 's/^ *"command": "//' -e 's/",$//' -e 's|-o [^ ]*|-o /dev/null|' |
    grep -v -- '-c .*/third_party/' |
    sort -u > "$WORK/cmds"

echo "analysing $(wc -l < "$WORK/cmds" | tr -d ' ') translation units with $JOBS jobs"

# ------------------------------------------------------------- the two passes -
#
# HOW MUCH THE ANALYSER IS ALLOWED TO EXPLORE
#
#   -Wanalyzer-too-complex is off by default, and when the analyser gives up on
#   a function it then says nothing at all about it.  At GCC's default
#   bb-explosion-factor of 5, 48 of this tree's 213 units gave up -- including
#   socket.c, select.c, tcp_handler.c, netdb.c, fetch.c, telnet.c and
#   nettrace.c -- which is to say three of the five defects the memory-safety
#   audit found were in files -fanalyzer had never actually looked at.  A run
#   that reports "clean" for those is worse than no run.
#
#   50 brings it down to five, for about 150 seconds.  The warning is turned ON
#   so the five that remain are printed rather than assumed away, and the two
#   that are worth more time get it by name:
#
#     netdb.c    3.2s at 200 -- parses DEVS:Internet/*, and is where the alias
#                pool overflow lived
#     nettrace.c 4.1s at 200 -- parses what comes back off the wire
#
#   Not raised: c68k_25519.c and c68k_mont.c (bignum limb loops, 18s each and
#   no pointer arithmetic over untrusted input) and tcpdrill.c (over eight
#   minutes at 200, and a test harness).
EXPLODE_DEFAULT=50
explode_for() {
    case "$1" in
        */src/config/netdb.c|*/src/tools/nettrace.c) echo 200 ;;
        *) echo "$EXPLODE_DEFAULT" ;;
    esac
}

run_one() {
    local cmd="$1" log="$2" src="${1##*-c }"
    local params
    params="-Wanalyzer-too-complex --param=analyzer-bb-explosion-factor=$(explode_for "$src")"

    # _NO_INLINE first, because that is the pass whose uninitialised-value
    # findings mean anything.  If the translation unit will not compile that
    # way, fall back rather than lose the file.  The files that fall back are
    # the ones that hand-roll an LPnNR() for a call the NDK has no prototype
    # for: those need inline/macros.h and EXEC_BASE_NAME, which _NO_INLINE
    # takes away.  They are still analysed, just with the NDK blind spot.
    if eval "$cmd $params -D_NO_INLINE" > "$log" 2>&1; then
        return 0
    fi
    eval "$cmd $params" > "$log" 2>&1 || true
    echo "$src" >> "$WORK/fellback"
}

i=0
: > "$WORK/fellback"
while IFS= read -r cmd; do
    i=$((i + 1))
    run_one "$cmd" "$WORK/log.$i" &
    [ $((i % JOBS)) -eq 0 ] && wait
done < "$WORK/cmds"
wait

if [ -s "$WORK/fellback" ]; then
    echo "$(sort -u "$WORK/fellback" | wc -l | tr -d ' ') unit(s) analysed without -D_NO_INLINE (NDK blind spot applies):"
    sed -e "s|$ROOT/||" "$WORK/fellback" | sort -u | sed 's/^/  /'
fi

# A unit that gave up was not checked.  Print it every run; it is the one
# number that says how much of the tree this stage actually covers.
grep -h 'analysis bailed out early' "$WORK"/log.* 2>/dev/null |
    sed -e "s|$ROOT/||" | cut -d: -f1 | grep -v '^cc1$' | sort -u > "$WORK/bailed" || true
if [ -s "$WORK/bailed" ]; then
    sed 's/^/NOT COVERED (too complex): /' "$WORK/bailed"
fi

# ------------------------------------------------------------ normalisation ---
#
#   <count> TAB <file> TAB <-Wanalyzer-name> TAB <message>
#
# Line and column are dropped on purpose; see the header.

export LC_ALL=C

# sort -u BEFORE the line number is dropped: the tools share sources between
# targets, so the same warning arrives once per target and only the file:line
# distinguishes a genuine second site from a rebuild of the first.
cat "$WORK"/log.* |
    grep -E 'warning: .*\[-Wanalyzer' |
    grep -v -- '-Wanalyzer-too-complex' |
    sed -e "s|$ROOT/||" |
    sort -u |
    sed -E -e 's| \[CWE-[0-9]+\]||' \
           -e 's|^([^:]+):[0-9]+:[0-9]+: warning: (.*) \[(-Wanalyzer[a-z-]*)\]$|\1\t\3\t\2|' |
    awk -F'\t' '{ n[$0]++ } END { for (k in n) printf "%d\t%s\n", n[k], k }' |
    sort > "$WORK/found"

if [ "$UPDATE" = 1 ]; then
    {
        echo "# GCC -fanalyzer findings that have been triaged and are not defects."
        echo "# Regenerate with tools/analyze.sh --update.  See the script header."
        echo "# count<TAB>file<TAB>warning<TAB>message"
        cat "$WORK/found"
    } > "$BASELINE"
    echo "baseline updated: $(wc -l < "$WORK/found" | tr -d ' ') entries"
    exit 0
fi

grep -v '^#' "$BASELINE" | grep -v '^$' | sort > "$WORK/base" || true

NEW=$(comm -23 "$WORK/found" "$WORK/base" || true)
GONE=$(comm -13 "$WORK/found" "$WORK/base" || true)

status=0
if [ -n "$NEW" ]; then
    printf '\033[31mnew -fanalyzer findings:\033[0m\n%s\n' "$NEW"
    status=1
fi
if [ -n "$GONE" ]; then
    printf '\033[33mbaseline entries that no longer occur -- rerun with --update:\033[0m\n%s\n' "$GONE"
    status=1
fi
[ "$status" = 0 ] && echo "-fanalyzer: $(wc -l < "$WORK/base" | tr -d ' ') known findings, no new ones"
exit "$status"
