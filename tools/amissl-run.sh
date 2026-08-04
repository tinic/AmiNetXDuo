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
# amisslmaster.library then opens `LIBS:AmiSSL/amissl_v362.library`, that
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
# AMINETXDUO_AMISSL_OS3 points at the unpacked OS3 release, the directory
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

# ------------------------------------------------------- the math libraries --
#
# AmiSSL is built against clib2, whose startup opens the IEEE math libraries,
# and Kickstart 3.1's ROM contains mathieeesingbas and NOTHING ELSE (verified
# against the 40.68 A1200 image, docs/RESEARCH.md 11.2).  A bare directory hard
# drive has none of the others.
#
# Two are needed, not one, and the second one cost an afternoon: AmiSSL's own
# `OpenSSL` command staged with only mathieeedoubbas.library prints
# "mathieeedoubtrans.library could not be opened." and exits 20.  That is the
# loud version of the failure.  The quiet version is what
# amissl_v362.library's own LibInit does with the same missing library, which
# is to sit there, the benchmark looked hung for a quarter of an hour before
# this probe was run.
#
# They must be a MATCHED PAIR.  Measured, with AmiSSL's own `OpenSSL` command
# as the probe: a stock mathieeedoubbas.library beside the AROS
# mathieeedoubtrans.library still reports "mathieeedoubtrans.library could not
# be opened"; both from the AROS m68k boot ISO and the command gets all the way
# to its own "Couldn't open bsdsocket.library v4!", which is that command's
# requirement and not AmiSSL's.  So build/amissl-mathlibs/ holds the AROS pair
# and is searched first.
#
# Getting them: the AROS m68k boot ISO carries all four in Libs/, and
# `bsdtar xf aros-amiga-m68k.iso Libs/mathieeedoub*.library` extracts them.
# Not vendored here, they are somebody else's binaries.
MATH_MISSING=0
for lib in mathieeedoubbas mathieeedoubtrans; do
    src=""
    for candidate in \
        "${AMINETXDUO_MATHLIBS:-}/$lib.library" \
        "$ROOT/build/amissl-mathlibs/$lib.library" \
        "$ROOT/build/$lib.library" \
        "$HOME/amigaos/libs/$lib.library"
    do
        case "$candidate" in /*.library) ;; *) continue ;; esac
        [ -f "$candidate" ] && { src="$candidate"; break; }
    done
    if [ -n "$src" ]; then
        cp "$src" "$STAGE/libs/"
        printf '    %-28s %8d bytes (from %s)\n' \
            "$lib.library" "$(wc -c < "$src")" "$src"
    else
        echo "    WARNING: no $lib.library found, AmiSSL's clib2 startup"
        echo "             will not complete.  Set AMINETXDUO_MATHLIBS."
        MATH_MISSING=1
    fi
done
[ "$MATH_MISSING" = "0" ] || echo "    (the run will hang rather than fail; see the comment in this script)"

# ------------------------------------------------------------- AmiSSL: ------
#
# AmiSSL is configured with OPENSSLDIR = AmiSSL: (its own Makefile:
# `OPENSSLDIR=AmiSSL: ENGINESDIR=AmiSSL:engines MODULESDIR=AmiSSL:modules`), so
# OpenSSL 3.x's configuration and provider loading opens AmiSSL:openssl.cnf on
# the first API call.  Without the assign AmigaDOS asks for the volume rather
# than returning an error, and on a bare boot nobody can cancel it.  The
# benchmark makes the assign itself from DH0:AmiSSL; this stages what it points
# at.  Certs/ is not staged, 290 PEM files that no arithmetic benchmark reads.
mkdir -p "$STAGE/AmiSSL"
if [ -f "$OS3/C/openssl.cnf" ]; then
    cp "$OS3/C/openssl.cnf" "$STAGE/AmiSSL/"
    echo "    AmiSSL:openssl.cnf staged"
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
        "$STAGE/libs" "$STAGE/env" "$STAGE/AmiSSL"
