#!/usr/bin/env bash
#
# NO SHIPPED COMMAND MAY SEND A USER TO A LOG IT CANNOT WRITE.
#
#   tests/tools/check-no-log-advice.sh [build-dir]
#
# THE CONSTRAINT.  AMINETXDUO_LOG is OFF in every drawer that ships -- that is
# the whole point of tools/check-no-diag-strings.sh, which keeps diagnostic
# sentences out of the resident images for size.  A shipping build therefore
# produces NO debug log at all.  A command that tells a user to go and read one
# is sending them to a file that cannot exist, and it costs them the evening
# they spend looking for it before they conclude the software is lying.
#
# WHAT IS RED TODAY, and it is why this file was written:
#
#   src/tools/addnetinterface.c
#     "Check the interface file for a wrong ADDRESS or CONFIGURE line.
#      Check the debug log for what failed after the device opened."
#
#   That is the last line a user sees when a card opens and the interface then
#   does not come online, which is one of the most common ways bring-up ends.
#
# WHY A STRINGS GREP AND NOT A SOURCE GREP.  The source is where the sentence
# is written and the BINARY is what ships; a sentence introduced by a macro, a
# table or a concatenation is in the second and not obviously in the first.
# tools/check-no-diag-strings.sh takes the same view for the same reason.  It
# needs a cross build and no emulator, so it is cheap and it is exact.
#
# WHY IT IS NOT tools/check-no-diag-strings.sh.  That one asks whether a
# sentence is in a RESIDENT image -- the library, the device -- and is about
# size.  This one asks whether a sentence is TRUE, in any shipped command, and
# is about the user.  The Shell commands are exactly where the first check
# does not look, on purpose, and exactly where the defect was.
#
# Output is key=value and an exit code: 0 clean, 1 a command sends the user to
# a log, 2 there is nothing built to check.
#
# SPDX-License-Identifier: MIT

set -uo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
BUILD="${1:-${AMINETXDUO_BUILD:-build/cm}}"
case "$BUILD" in /*) ;; *) BUILD="$ROOT/$BUILD" ;; esac

TOOLDIR="$BUILD/src/tools"
if [ ! -d "$TOOLDIR" ]; then
    echo "log_advice=skipped reason=no_build dir=$TOOLDIR"
    exit 2
fi

# The sentence shapes that mean the noun.  Same list as
# tests/tools/bringupfail-verdict.sh and for the same reason: "login",
# "logical", "dialog" and "catalogue" are words, and a check that flagged them
# would be switched off within a week.
PATTERN='(debug|the|a|error|serial|trace)[[:space:]]+log([[:space:]]|file|s|\.|,|$)|log[[:space:]]*file|check[[:space:]]+the[[:space:]]+log|enable[[:space:]]+logging|turn[[:space:]]+on[[:space:]]+logging|see[[:space:]]+the[[:space:]]+log'

# EVERY command, not a list.  A list would need keeping in step with
# src/tools/CMakeLists.txt, and the one that got added without being added to
# the list is the one that would carry the sentence.
bad=0
checked=0
for f in "$TOOLDIR"/*; do
    [ -f "$f" ] || continue
    case "$f" in
        *.map|*.o|*.a|*.cmake|*Makefile*) continue ;;
    esac
    # Executables only: an AmigaOS hunk binary has no exec bit here reliably,
    # so the test is "does `strings` find anything at all".
    hits=$(strings -n 6 "$f" 2>/dev/null | grep -inE "$PATTERN" || true)
    checked=$((checked + 1))
    [ -n "$hits" ] || continue

    while IFS= read -r line; do
        [ -n "$line" ] || continue
        echo "log_advice_found=$(basename "$f") text=${line#*:}"
        bad=$((bad + 1))
    done <<EOF
$hits
EOF
done

echo "log_advice_commands_checked=$checked"
echo "log_advice_offences=$bad"

if [ "$bad" = 0 ]; then
    echo "log_advice=clean"
    exit 0
fi

echo "log_advice=FOUND"
echo >&2
echo "A shipped command tells the user to consult a log." >&2
echo >&2
echo "  AMINETXDUO_LOG is off in every shipping drawer, so no such log is" >&2
echo "  ever written.  The advice cannot be followed by the person being" >&2
echo "  given it." >&2
echo >&2
echo "  Replace the sentence with something the user can ACT on: the" >&2
echo "  operation that failed, its code, and the file or the line to change." >&2
echo "  tests/tools/bringupfail-verdict.sh is the contract, and its selftest" >&2
echo "  has a worked example for each of the five causes." >&2
exit 1
