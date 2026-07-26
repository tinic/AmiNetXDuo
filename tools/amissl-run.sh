#!/usr/bin/env bash
#
# Run tests/crypto68k/crypto68k_amissl under FS-UAE with AmiSSL staged.
#
#   tools/amissl-run.sh [-t SECONDS] [-k MHZ] [-f] [EXE]
#
#   -f  set C68K_AMISSL=full, which adds the slow extras (an AmiSSL RSA-2048
#       private operation WITHOUT CRT, which is minutes on its own)
#
# WHY THIS SCRIPT EXISTS RATHER THAN A LINE IN THE README
#
# The emulated machine resolves amissl.library through LIBS:, and
# amisslmaster.library then opens `LIBS:AmiSSL/amissl_v362.library` -- that
# path is a literal inside amisslmaster (checked with `strings`), so the
# directory layout is not negotiable.  Getting it wrong produces
# "OpenAmiSSLTags failed", which says nothing about which of the two files was
# missing.  So the staging happens here, once, and the script says which build
# of the library it copied.
#
# WHICH CPU BUILD.  AmiSSL ships exactly two m68k builds: `68020-40`, compiled
# `-m68020-40 -msoft-float` WITH Howard Chu's bn_m68k.s bignum assembly, and
# `68060`, compiled `-m68060` WITHOUT it (MULU.L's 32x32->64 form is not
# implemented on a 68060 and traps to the emulator, so the assembly is a
# pessimisation there and AmiSSL disabled it in 4.4).  The timing profile is a
# 68EC020, so `68020-40` is the correct and the only defensible choice, and it
# is the one that gives OpenSSL its best case.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

TIMEOUT=1200
CLOCK=""
FULL=0

while getopts "t:k:f" opt; do
    case "$opt" in
        t) TIMEOUT="$OPTARG" ;;
        k) CLOCK="$OPTARG" ;;
        f) FULL=1 ;;
        *) echo "usage: $0 [-t seconds] [-k MHz] [-f] [exe]" >&2; exit 2 ;;
    esac
done
shift $((OPTIND - 1))

EXE="${1:-$ROOT/build/cm-tls/tests/crypto68k/crypto68k_amissl}"
[ -f "$EXE" ] || {
    echo "no such executable: $EXE" >&2
    echo "build it with -DAMINETXDUO_AMISSL_SDK=<sdk>/AmiSSL/Developer" >&2
    exit 2
}

# ------------------------------------------------------------- the library --
#
# AMINETXDUO_AMISSL_OS3 points at the unpacked OS3 release -- the directory
# that holds Libs/AmigaOS3.  Searched rather than required, because the two
# archives usually land side by side.
CANDIDATES=(
    "${AMINETXDUO_AMISSL_OS3:-}"
    "$ROOT/build/amissl/AmiSSL"
    "$HOME/Downloads/AmiSSL"
)
OS3=""
for c in "${CANDIDATES[@]}"; do
    [ -n "$c" ] || continue
    if [ -d "$c/Libs/AmigaOS3/AmiSSL" ]; then OS3="$c"; break; fi
done

[ -n "$OS3" ] || {
    echo "Could not find an unpacked AmiSSL OS3 release." >&2
    echo "Set AMINETXDUO_AMISSL_OS3 to the directory holding Libs/AmigaOS3, e.g." >&2
    echo "  curl -LO https://github.com/jens-maus/amissl/releases/download/5.27/AmiSSL-5.27-OS3.lha" >&2
    echo "  lha x AmiSSL-5.27-OS3.lha && export AMINETXDUO_AMISSL_OS3=\$PWD/AmiSSL" >&2
    exit 2
}

MASTER="$OS3/Libs/AmigaOS3/amisslmaster.library"
CPUDIR="$OS3/Libs/AmigaOS3/AmiSSL/68020-40"

[ -f "$MASTER" ] || { echo "missing $MASTER" >&2; exit 2; }
[ -d "$CPUDIR" ] || { echo "missing $CPUDIR" >&2; exit 2; }

STAGE="$ROOT/build/amissl-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs/AmiSSL"
cp "$MASTER" "$STAGE/libs/"
cp "$CPUDIR"/amissl_v*.library "$STAGE/libs/AmiSSL/"

echo "==> AmiSSL staged from $OS3"
for f in "$STAGE/libs/amisslmaster.library" "$STAGE/libs/AmiSSL"/*.library; do
    printf '    %-28s %8d bytes\n' "$(basename "$f")" "$(wc -c < "$f")"
done
echo "    CPU build: 68020-40 (the one with bn_m68k.s; the 68060 build has none)"

# AmiSSL is built against clib2, whose startup initialises the IEEE math
# libraries, and Kickstart 3.1's ROM contains mathieeesingbas and nothing else
# (verified against the 40.68 A1200 image, docs/RESEARCH.md 11.2).  Every
# Workbench install has mathieeedoubbas.library in LIBS: and a bare directory
# hard drive does not, so stage one if we can find it.  Same search as
# clients/curl/run-fsuae.sh.
MATH="${AMINETXDUO_MATHIEEEDOUBBAS:-}"
if [ -z "$MATH" ]; then
    for candidate in \
        "$ROOT/build/mathieeedoubbas.library" \
        "$HOME/amigaos/libs/mathieeedoubbas.library"
    do
        [ -f "$candidate" ] && { MATH="$candidate"; break; }
    done
fi
if [ -n "$MATH" ]; then
    cp "$MATH" "$STAGE/libs/"
    echo "    mathieeedoubbas.library staged from $MATH"
else
    echo "    WARNING: no mathieeedoubbas.library staged; clib2's math init"
    echo "             may fail inside AmiSSL.  Set AMINETXDUO_MATHIEEEDOUBBAS."
fi

# C68K_AMISSL is read with GetVar(), which reads ENV:.  The harness backs ENV:
# with a staged env/ directory.
mkdir -p "$STAGE/env"
if [ "$FULL" = "1" ]; then
    printf 'full' > "$STAGE/env/C68K_AMISSL"
    echo "    mode: full (includes AmiSSL's non-CRT private operation)"
else
    printf 'quick' > "$STAGE/env/C68K_AMISSL"
    echo "    mode: quick"
fi

# ------------------------------------------------------------------- run ---

ARGS=(-t "$TIMEOUT")
[ -z "$CLOCK" ] || ARGS+=(-k "$CLOCK")

AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-amissl}" \
    "$ROOT/tools/fsuae-run.sh" "${ARGS[@]}" "$EXE" \
        "$STAGE/libs" "$STAGE/env"
