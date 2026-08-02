#!/usr/bin/env bash
#
# Every installer scenario, in order, under FS-UAE.
#
#   install/test/run-all.sh [-b BUILDDIR]
#
# Each scenario is a full install onto a freshly staged bare machine:
#
#   NOVICE    no questions asked at all -- the card is auto-detected from
#             DEVS: and every default has to be right on its own
#   AVERAGE   the questions a normal install asks, all answered with the
#             default
#   EXPERT    the same plus the unit number, the interface name and the
#             confirmation page for every copy and every file written
#   STATIC    "no" to the DHCP question, which is the only way into the four
#             validated address prompts; checks the files, not the network
#   RERUN     installs twice over itself; the second pass has to notice the
#             existing configuration and keep it
#   SHARE     "yes" to the file server, so S:User-Startup gets its line too
#   SHARERERUN  the same twice over, where a block that grew instead of being
#             replaced would show up as two of everything
#   REMOVE    takes both lines, then installs again and declines both; what
#             the installer added it has to be able to take away
#
# It starts with ICONS, which hands the generated .info files to the real
# icon.library rather than trusting the generator to grade its own homework.
#
# Every scenario checks S:User-Startup line by line.  All but the three that
# stop there then boot the installed machine with an A2065 on SLIRP and
# require the network to come up from S:User-Startup alone.
#
# Runs are serialised by tools/fsuae-run.sh's lock, so this takes a while --
# budget about fifteen minutes.
#
# SPDX-License-Identifier: MIT

set -uo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ARGS=("$@")

SCENARIOS=(NOVICE AVERAGE EXPERT STATIC RERUN SHARE SHAREONLY SHARERERUN
           REMOVE)
declare -a RESULTS

ROOT=$(cd "$HERE/../.." && pwd)
GCC="${AMIGA_GCC:-$HOME/amigaos/tools/m68k-amigaos-gcc/bin/m68k-amigaos-gcc}"
NDK="${AMIGA_NDK:-$HOME/amigaos/tools/m68k-amigaos-gcc/m68k-amigaos/ndk-include}"

failures=0

# --------------------------------------------------------------- the icons --
#
# Cheap and worth doing first: hand the generated .info files to the real
# icon.library rather than trusting the generator and its own reader to agree
# with each other.

echo
echo "############################################################"
echo "#  ICONS"
echo "############################################################"
python3 "$ROOT/install/tools/makeicon.py" "$ROOT/install" >/dev/null
python3 "$ROOT/install/tools/showicon.py" "$ROOT/install"/*.info >/dev/null
"$GCC" -O2 -m68020 -Wall -Wextra -I"$NDK" \
       -o "$ROOT/build/icontest" "$ROOT/install/test/icontest.c"
rm -rf "$ROOT/build/icons-stage"
mkdir -p "$ROOT/build/icons-stage/AmiNetXDuo"
cp "$ROOT/install"/*.info "$ROOT/build/icons-stage/AmiNetXDuo/"
if AMINETXDUO_RUN_TAG=icons "$ROOT/tools/fsuae-run.sh" -t 90 \
        "$ROOT/build/icontest" "$ROOT/build/icons-stage/AmiNetXDuo"; then
    RESULTS+=("  PASS  ICONS")
else
    RESULTS+=("  FAIL  ICONS")
    failures=$((failures + 1))
fi
for scenario in "${SCENARIOS[@]}"; do
    echo
    echo "############################################################"
    echo "#  $scenario"
    echo "############################################################"
    if "$HERE/run-installer-fsuae.sh" -l "$scenario" -t 280 "${ARGS[@]}"; then
        RESULTS+=("  PASS  $scenario")
    else
        RESULTS+=("  FAIL  $scenario")
        failures=$((failures + 1))
    fi
done

echo
echo "============================================================"
echo "  summary"
echo "============================================================"
printf '%s\n' "${RESULTS[@]}"
echo
if [ "$failures" = "0" ]; then
    echo "==> everything passed"
else
    echo "==> $failures check(s) failed"
fi
exit "$failures"
