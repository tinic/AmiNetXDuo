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
#       below, the NDK blind spot this script otherwise removes.
#
#   Silencing those would mean adding dead NULL tests, pragmas over whole
#   functions, or a rewrite of a test's formatter, all of which cost more
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
#   only that, the same call declared as a plain extern produces none.
#
#   <proto/*.h> already has the switch: _NO_INLINE takes the clib/*_protos.h
#   prototypes instead of the inline macros.  Objects built that way would not
#   link, which is why this pass compiles to /dev/null and the real build is
#   left alone.  A handful of files do not compile under it, they hand-roll
#   an LPnNR() for a call the NDK has no prototype for, and those fall back
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

# The configure log goes beside the build directory, so its parent has to
# exist: in a clean checkout there is no build/ yet and this failed with
# "build/analyze-configure.log: No such file or directory" before cmake was
# even reached.  A tool that cannot run in a fresh clone is a tool that only
# runs where somebody has already built.
mkdir -p "$(dirname "$BUILD")"

cmake -S . -B "$BUILD" \
    -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-m68k-amigaos.cmake \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DAMINETXDUO_LTO=OFF \
    -DCMAKE_C_FLAGS=-fanalyzer > "$BUILD-configure.log" 2>&1 || {
        tail -30 "$BUILD-configure.log"; exit 1; }
# AMINETXDUO_LTO=OFF because -flto SILENTLY DISABLES -fanalyzer.  A unit that
# reports a use-of-uninitialised-value on its own reports nothing with -flto
# added -- no note, no warning, no diagnostic of any kind.  This stage would
# have kept running, found zero of everything, and said so in a way that reads
# like good news.  It is not enough to leave -flto off the command line here:
# it arrives through CMAKE_C_FLAGS_RELEASE, which is FORCEd, so the switch has
# to be the one the top-level CMakeLists.txt tests.

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# compile_commands.json, one command per line, ours only, deduplicated.
#
# run_one() eval()s these, and CMake has already shell-quoted what it put in
# the JSON string, so the only layer to undo here is the JSON one.  Two
# escapes come out of CMake and both are decoded below, backslash first, the
# order JSON defines:
#
#   \\  ->  \    a backslash the shell wants, which eval then consumes
#   \"  ->  "    likewise part of the shell quoting, not of the argument
#
# A quoted -D is what produces them.  -DATF_PROGNAME="tcp_socket" arrives as
# -DATF_PROGNAME=\\\"tcp_socket\\\", decodes to -DATF_PROGNAME=\"tcp_socket\",
# and eval hands the compiler -DATF_PROGNAME="tcp_socket", so the macro is
# still a string literal.  Checked end to end, not reasoned about.
#
# Anything else -- \n, \t, \/, \uXXXX -- is still refused, because decoding it
# properly means a real JSON parser.  What changed is that the refusal now
# names the argument that caused it: the old message said only "this parser is
# too naive", which named the file and not the cause, and cost a CI run on
# 2026-08-10 when tests/atf/CMakeLists.txt first added a quoted -D.
#
# The two handled escapes are removed BEFORE looking for a leftover backslash.
# Searching for \<other> directly reports the second half of every \\ as a
# finding that is not there.
if sed -e 's/\\\\//g' -e 's/\\"//g' "$BUILD/compile_commands.json" |
       grep -q '\\'; then
    echo "compile_commands.json: a JSON escape this parser does not decode" >&2
    echo "  handled: \\\\ and \\\" -- anything else needs a real JSON parser" >&2
    echo "  the argument(s) that produced it:" >&2
    sed -e 's/\\\\/\x01/g' -e 's/\\"/\x02/g' "$BUILD/compile_commands.json" |
        grep -o '[^ ]*\\[^ ]*' | sort -u | head -20 |
        sed -e 's/\x01/\\\\/g' -e 's/\x02/\\"/g' -e 's/^/    /' >&2
    echo "  in $BUILD/compile_commands.json" >&2
    exit 1
fi
grep '"command":' "$BUILD/compile_commands.json" |
    sed -e 's/^ *"command": "//' -e 's/",$//' -e 's|-o [^ ]*|-o /dev/null|' \
        -e 's/\\\\/\\/g' -e 's/\\"/"/g' |
    grep -v -- '-c .*/third_party/' |
    sort -u > "$WORK/cmds"

echo "analysing $(wc -l < "$WORK/cmds" | tr -d ' ') translation units with $JOBS jobs"

# ------------------------------------------------------------- the two passes -
#
# HOW MUCH THE ANALYSER IS ALLOWED TO EXPLORE
#
#   -Wanalyzer-too-complex is off by default, and when the analyser gives up on
#   a function it then says nothing at all about it.  At GCC's default
#   bb-explosion-factor of 5, 48 of this tree's 213 units gave up, including
#   socket.c, select.c, tcp_handler.c, netdb.c, fetch.c, telnet.c and
#   nettrace.c, which is to say three of the five defects the memory-safety
#   audit found were in files -fanalyzer had never actually looked at.  A run
#   that reports "clean" for those is worse than no run.
#
#   50 brings it down to five, for about 150 seconds.  The warning is turned ON
#   so the five that remain are printed rather than assumed away, and the two
#   that are worth more time get it by name:
#
#     netdb.c    3.2s at 200, parses DEVS:Internet/*, and is where the alias
#                pool overflow lived
#     nettrace.c 4.1s at 200, parses what comes back off the wire
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

#
# A pool, not batches. `wait` every JOBS units made each batch cost its slowest
# member: 240 units averaging 1.25 s took 253 s wall, because a handful of slow
# files landed in different batches and 23 idle cores waited for each one. With
# `wait -n` a finished slot is refilled immediately, so the floor is the single
# slowest unit rather than the sum of ten worst-in-batch.
#
i=0
running=0
: > "$WORK/fellback"
while IFS= read -r cmd; do
    i=$((i + 1))
    run_one "$cmd" "$WORK/log.$i" &
    running=$((running + 1))
    if [ "$running" -ge "$JOBS" ]; then
        wait -n
        running=$((running - 1))
    fi
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
# `|| true' on the first grep: with no matches it exits 1, and under pipefail
# that killed the script here, silently, with status 1 and not a word printed.
# It happened for real -- -flto had turned every finding off and the stage died
# at this line looking like an unrelated tooling fault.  An empty result is a
# legitimate answer and belongs in the comparison below, where it shows up as
# every baseline entry being GONE, which is loud and says what happened.
cat "$WORK"/log.* |
    { grep -E 'warning: .*\[-Wanalyzer' || true; } |
    { grep -v -- '-Wanalyzer-too-complex' || true; } |
    sed -e "s|$ROOT/||" |
    sort -u |
    sed -E -e 's| \[CWE-[0-9]+\]||' \
           -e 's|^([^:]+):[0-9]+:[0-9]+: warning: (.*) \[(-Wanalyzer[a-z-]*)\]$|\1\t\3\t\2|' |
    awk -F'\t' '{ n[$0]++ } END { for (k in n) printf "%d\t%s\n", n[k], k }' |
    sort > "$WORK/found"

grep -v '^#' "$BASELINE" | grep -v '^$' | sort > "$WORK/base" || true

# A unit that gave up was NOT checked, so nothing about it can be compared:
# its findings are whatever the analyser managed before it stopped, and its
# baseline entries were never looked for.  Set both aside.
#
# Which units bail is not a property of the tree.  src/tools/httpd.c bailed on
# the CI runner and was analysed in full on another host, same commit and the
# same pinned GCC, and the two -Wanalyzer-use-of-uninitialized-value findings it
# then produced (DateStamp() fills the struct through a library call the
# analyser cannot see) would have failed the build as NEW on one host while the
# same entries failed as GONE on the other.  A coverage difference is not a code
# change and must not read as one.
bailed_filter() {
    if [ -s "$WORK/bailed" ]; then
        awk -F'\t' 'NR==FNR { bail[$0]=1; next } !($2 in bail)' \
            "$WORK/bailed" "$1"
    else
        cat "$1"
    fi
}
bailed_only() {
    if [ -s "$WORK/bailed" ]; then
        awk -F'\t' 'NR==FNR { bail[$0]=1; next } ($2 in bail)' \
            "$WORK/bailed" "$1"
    fi
}

if [ "$UPDATE" = 1 ]; then
    #
    # RUN --update ON LINUX, NOT ON A MAC.  The bailed/set-aside machinery
    # above covers a unit that gave up, which is the coverage case.  It does
    # NOT cover a unit that is analysed on both hosts and reports differently
    # on each, and that happens: src/netstack/netstack.c yields a
    # use-of-uninitialized-value on the Linux runner and nothing at all on
    # darwin-arm64, with the same pinned GCC -- the analyser is built by a
    # different host compiler on each, and its state limits do not land in the
    # same place. Updating from a Mac silently drops that entry, and CI then
    # fails it as NEW. It cost a red build to learn; Linux is where CI runs, so
    # Linux is what the baseline has to describe.
    #
    # Carry forward the baseline entries for units that bailed on THIS run.
    # Dropping them would make the next host that manages to analyse one fail
    # on findings that were triaged long ago.
    {
        echo "# GCC -fanalyzer findings that have been triaged and are not defects."
        echo "# Regenerate with tools/analyze.sh --update.  See the script header."
        echo "# count<TAB>file<TAB>warning<TAB>message"
        { bailed_filter "$WORK/found"; bailed_only "$WORK/base"; } | sort
    } > "$BASELINE"
    echo "baseline updated: $(grep -vc '^#' "$BASELINE" | tr -d ' ') entries"
    exit 0
fi

bailed_filter "$WORK/found" > "$WORK/found.cmp"
bailed_filter "$WORK/base"  > "$WORK/base.cmp"
SETASIDE=$(bailed_only "$WORK/base")

NEW=$(comm -23 "$WORK/found.cmp" "$WORK/base.cmp" || true)
GONE=$(comm -13 "$WORK/found.cmp" "$WORK/base.cmp" || true)

status=0
if [ -n "$NEW" ]; then
    printf '\033[31mnew -fanalyzer findings:\033[0m\n%s\n' "$NEW"
    status=1
fi
if [ -n "$GONE" ]; then
    printf '\033[33mbaseline entries that no longer occur -- rerun with --update:\033[0m\n%s\n' "$GONE"
    status=1
fi
if [ -n "$SETASIDE" ]; then
    printf 'set aside, their unit was not analysed this run:\n%s\n' "$SETASIDE"
fi
[ "$status" = 0 ] && echo "-fanalyzer: $(wc -l < "$WORK/base.cmp" | tr -d ' ') known findings, no new ones"
exit "$status"
