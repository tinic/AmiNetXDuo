#!/usr/bin/env bash
#
# Run cppcheck over our own sources and compare with tools/cppcheck-baseline.txt.
#
#   tools/cppcheck.sh            # check: new findings are a failure
#   tools/cppcheck.sh --update   # rewrite the baseline from this run
#   tools/cppcheck.sh --style    # add the style class, print, gate nothing
#
# GETTING THE TARGET RIGHT IS MOST OF THE WORK
#
#   Three things have to be handed to cppcheck or it invents findings and
#   misses whole subsystems:
#
#   1. The platform.  m68k-amigaos is ILP32 big-endian with signed plain char
#      and 2-byte struct alignment.  None of cppcheck's built-in platforms is
#      that, so cmake/cppcheck-m68k.xml describes it.
#
#   2. The predefined macros.  Without them <machine/ieeefp.h> stops at
#      "#error Endianess not declared!!" and <sys/_intsup.h> at "Unable to
#      determine type definition of intptr_t", and every translation unit that
#      reaches either is abandoned.  They are dumped from the real compiler at
#      run time rather than checked in, so they cannot drift from it.
#
#   3. -D'__asm(x)='.  cppcheck cannot parse GCC's register-variable syntax --
#      `register ULONG version __asm("d0")`, which is how every library vector
#      in this tree is declared, and abandons the file at the first one.
#      With the NDK's real headers on the include path and this define, 32
#      files that used to be silently skipped are checked, src/bsdsocket among
#      them.  The define is safe because a function-like macro only expands
#      when the next token is `(`, and statement asm is spelled
#      `__asm volatile (...)`.
#
# WHY ONLY error AND warning ARE GATED
#
#   With style, performance and portability on, this tree produces 1,059
#   findings.  1,046 of them are constVariablePointer, variableScope,
#   unusedStructMember and friends: they are a house-style argument, not a
#   defect report, and a baseline of a thousand of them would never be read.
#   All thirteen of the error/warning findings were triaged and are false --
#   see the baseline for what each one is, but that class is small enough to
#   watch, so it is the one this gate watches.  --style prints the rest.
#
# NOT INSTALLED IN CI, ON PURPOSE
#
#   cppcheck is not part of the pinned toolchain, and its findings move between
#   its own releases: a runner's distro cppcheck would disagree with the
#   baseline and turn this into a "regenerate it" ritual.  tools/ci.sh runs this
#   where it is installed and says out loud when it is not.  The gate CI relies
#   on is tools/analyze.sh, which uses the pinned compiler and is deterministic.
#   The baseline records which cppcheck produced it.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"

BASELINE="tools/cppcheck-baseline.txt"
BUILD="${AMINETXDUO_ANALYZE_BUILD:-build/analyze}"
JOBS="${AMINETXDUO_CI_JOBS:-$( (command -v nproc >/dev/null && nproc) || sysctl -n hw.ncpu 2>/dev/null || echo 4 )}"
ENABLE="warning"
UPDATE=0

for arg in "$@"; do
    case "$arg" in
        --update) UPDATE=1 ;;
        --style)  ENABLE="warning,style,performance,portability" ;;
        *) echo "usage: tools/cppcheck.sh [--update] [--style]" >&2; exit 2 ;;
    esac
done

command -v cppcheck > /dev/null || { echo "cppcheck not installed" >&2; exit 2; }
[ -n "${AMIGA_TOOLCHAIN_ROOT:-}" ] || . tools/amiga-toolchain.sh

CC="$AMIGA_TOOLCHAIN_ROOT/bin/m68k-amigaos-gcc"
[ -f "$BUILD/compile_commands.json" ] || {
    cmake -S . -B "$BUILD" \
        -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-m68k-amigaos.cmake \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON > /dev/null; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
export LC_ALL=C

# (2) above.  __STDC__ and the like are dropped: cppcheck sets its own and
# redefining them from the command line is a warning per file.
"$CC" -dM -E -Os -xc /dev/null |
    grep -vE '^#define (__STDC__|__STDC_VERSION__|__STDC_HOSTED__|__FILE__|__LINE__|__DATE__|__TIME__|__has_|__STDC_UTF)' \
    > "$WORK/predef.h"

SYSINC=$("$CC" -E -Wp,-v -xc /dev/null 2>&1 |
         sed -n '/search starts here/,/End of search/p' | grep '^ /' |
         sed 's|^ |-I|' | tr '\n' ' ')

# Our sources only, one entry per distinct command, with the toolchain's own
# include path and (3) appended.  cppcheck ignores -I/-D given alongside
# --project, so they have to go into the entries themselves.
EXTRA=" -D'__asm(x)=' $SYSINC" python3 - "$BUILD/compile_commands.json" "$WORK/cc.json" <<'PY'
import json, os, sys
extra = os.environ["EXTRA"]
seen, out = set(), []
for e in json.load(open(sys.argv[1])):
    if "/third_party/" in e["file"]:
        continue
    e["command"] += extra
    key = (e["file"], e["command"])
    if key in seen:
        continue
    seen.add(key)
    out.append(e)
json.dump(out, open(sys.argv[2], "w"))
PY

cppcheck --project="$WORK/cc.json" \
         --platform="$ROOT/cmake/cppcheck-m68k.xml" \
         --include="$WORK/predef.h" \
         --enable="$ENABLE" --inline-suppr \
         --suppress=missingIncludeSystem --suppress=checkersReport \
         -j "$JOBS" \
         --template='{file}:{line}: {severity}: {id}: {message}' \
         > /dev/null 2> "$WORK/raw" || true

# Keyed on file + id + message, without the line number, for the same reason
# tools/analyze.sh does it: a baseline that moves when an unrelated line moves
# above it gets regenerated blindly, and then it is not a gate.
sed -e "s|$ROOT/||" "$WORK/raw" |
    grep -v "^$AMIGA_TOOLCHAIN_ROOT" |
    sed -E 's|^([^:]+):[0-9]+:[0-9]*:? |\1\t|' |
    sort -u |
    awk -F'\t' '{ n[$0]++ } END { for (k in n) printf "%d\t%s\n", n[k], k }' |
    sort > "$WORK/found"

if [ "$ENABLE" != "warning" ]; then
    echo "$(awk -F'\t' '{s+=$1} END {print s+0}' "$WORK/found") findings, all classes:"
    awk -F'\t' '{ split($3, a, ":"); n[a[2]] += $1 } END { for (k in n) printf "%6d %s\n", n[k], k }' "$WORK/found" | sort -rn
    exit 0
fi

if [ "$UPDATE" = 1 ]; then
    {
        echo "# Produced by $(cppcheck --version)."
        cat <<'HDR'
# cppcheck error/warning findings that have been triaged and are not defects.
# Regenerate with tools/cppcheck.sh --update.  Every entry below is one of:
#
#   * an NDK inline out-parameter.  ReadEClock(&ev), DateStamp(&ds),
#     Read(fh,&c,1) and FGets() are `jsr` inside __asm volatile, so no analyser
#     sees the store.  All the uninitvar entries are this.
#   * a ReadArgs() /A argument.  ReadArgs writes the array and returns NULL if
#     a /A is missing, so args[0] is non-NULL past the check, clockset.c.
#   * NX_ASSERT, which is a no-op under NDEBUG.  cppcheck reads the assert as
#     the NULL test it then says is redundant, n68k_checksum.c and the
#     rfc7905 copy, both of which mirror vendored NetX Duo code.
#   * dead under this configuration, live under another: c68k_crt.c's last
#     scratch advance is the vendored carving kept deliberately intact.
#   * a bound cppcheck cannot prove: c68k_chacha20.c's n is 1..15 by
#     construction, ami_udivdi3.c shifts a 32-bit value by at most 31 (legal,
#     and a right shift at that), ami_random.c takes the ADDRESS of an
#     uninitialised local for entropy and never reads it.
#   * tests/tls/tls_resume.c: a static tag array pointing at an automatic that
#     is alive for the whole call and rebuilt on the next one.
#
# count<TAB>file<TAB>finding
HDR
        cat "$WORK/found"
    } > "$BASELINE"
    echo "baseline updated: $(wc -l < "$WORK/found" | tr -d ' ') entries"
    exit 0
fi

# Findings from a different cppcheck are not comparable with these, versions
# differ by hundreds of syntaxError lines on files nobody touched, and diffing
# them buries a real regression in noise.  Say so and stop rather than report a
# failure that means nothing: the stage has been red on the gate host for a
# while for exactly this reason, and a stage nobody believes is worse than one
# that does not run.
BASE_VER=$(sed -n 's/^# Produced by Cppcheck \(.*\)\.$/\1/p' "$BASELINE")
HAVE_VER=$(cppcheck --version 2>/dev/null | sed 's/^Cppcheck //')
if [ -n "$BASE_VER" ] && [ "$BASE_VER" != "$HAVE_VER" ]; then
    printf '\033[33mcppcheck %s here, baseline from %s, not comparable, skipping.\033[0m\n' \
           "${HAVE_VER:-unknown}" "$BASE_VER"
    echo "Install $BASE_VER, or regenerate with tools/cppcheck.sh --update."
    exit 0
fi

grep -v '^#' "$BASELINE" | grep -v '^$' | sort > "$WORK/base" || true
NEW=$(comm -23 "$WORK/found" "$WORK/base" || true)
GONE=$(comm -13 "$WORK/found" "$WORK/base" || true)

status=0
if [ -n "$NEW" ]; then
    printf '\033[31mnew cppcheck findings:\033[0m\n%s\n' "$NEW"; status=1
fi
if [ -n "$GONE" ]; then
    printf '\033[33mbaseline entries that no longer occur -- rerun with --update:\033[0m\n%s\n' "$GONE"
    status=1
fi
[ "$status" = 0 ] && echo "cppcheck: $(wc -l < "$WORK/base" | tr -d ' ') known findings, no new ones"
exit "$status"
