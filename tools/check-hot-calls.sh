#!/usr/bin/env bash
#
# The per-frame receive helpers must still be INLINED into their caller.
#
#   tools/check-hot-calls.sh <build-dir>
#
# WHY THIS EXISTS.  Two changes were proposed on 2026-09-07 on the strength of
# the sampling profiler naming a symbol.  One was real and measured +1.80 per
# cent; the other would have bought NOTHING, because the profiler build is
# LTO=OFF by construction and the functions it named -- bsd_iov_chunk,
# bsd_iov_advance, bsd_packet_extract -- are already inlined in the shipped
# image.  The only way to tell the two apart was to disassemble a build with
# its symbols kept and count the jsr sites.  That check is this file.
#
# It is a REGRESSION gate, not a discovery tool: it says nothing about which
# helper is worth inlining.  What it stops is a refactor -- a `static` dropped,
# a helper moved to another translation unit, an option that splits an LTO
# partition -- silently putting a call back on a path that runs 480 times a
# second, where the last one measured 1.8 per cent.
#
# A NAME HERE MUST BE A FUNCTION THE RECEIVE PATH CALLS PER FRAME OR PER DRAIN
# and that is currently inlined.  Adding one that is legitimately out of line
# makes this gate a liar; there is a separate list for those below.
#
# SPDX-License-Identifier: MIT

set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${1:?build directory}"
LIB="$BUILD/src/bsdsocket/bsdsocket.library"

# Inlined today, and each one is on the per-frame receive path.
MUST_BE_INLINED="
_bsd_packet_extract
_bsd_iov_chunk
_bsd_iov_advance
_ami_sana2_rx_deliver
_ami_sana2_rx_resolve_length
_ami_sana2_copy_to_buff
_ami_sana2_rx_should_block
_ami_bpf_tap_rx
"

# Out of line on purpose, and NOT a defect: big bodies, or reached through a
# pointer the linker cannot see through.  Listed so the next reader does not
# take their absence above for an oversight.
#   _n68k_rx_verify_sum   one call a frame, a large function with two families
#   _ami_sana2_rx_direct  SANA-II tag hook, called by the device by pointer
#   _ami_sana2_rx_filled  the same
#   _n68k_copy_bytes      the bulk copy itself

if [ ! -f "$LIB" ]; then
    echo "hot_calls=skipped reason=no_library path=$LIB"
    exit 0
fi

NM="$(command -v m68k-amigaos-nm || true)"
if [ -z "$NM" ] && [ -n "${AMIGA_TOOLCHAIN_ROOT:-}" ]; then
    NM="$AMIGA_TOOLCHAIN_ROOT/bin/m68k-amigaos-nm"
fi
OBJDUMP="${NM%nm}objdump"

if [ -z "$NM" ] || [ ! -x "$NM" ] || [ ! -x "$OBJDUMP" ]; then
    echo "hot_calls=skipped reason=no_toolchain"
    exit 0
fi

# SHIPPING-SHAPED ONLY.  The claim is about the image users get, and an
# instrumented arm is a different program: the `instr` arm carries RXPROBE,
# NXCENSUS, SCHEDCOUNT, SANA2_PROBE_RAW and the profiler, and in that shape
# _ami_bpf_tap_rx really is called out of line.  Asserting the shipping
# property against it made this gate red on its first run for a reason that was
# not a defect -- the arm, not the code.  The `symbols` arm is shipping options
# plus a symbol table, and is the one that answers.
CACHE="$BUILD/CMakeCache.txt"
if [ -r "$CACHE" ]; then
    for opt in PROFILER PROFILER_NOINLINE RXPROBE NXCENSUS SCHEDCOUNT \
               SANA2_PROBE_RAW NETDEV_TIME ALLOCCENSUS; do
        if grep -q "^AMINETXDUO_$opt:BOOL=ON" "$CACHE"; then
            echo "hot_calls=skipped reason=instrumented opt=$opt"
            exit 0
        fi
    done
    if grep -q "^AMINETXDUO_LTO:BOOL=OFF" "$CACHE"; then
        echo "hot_calls=skipped reason=lto_off"
        exit 0
    fi
fi

# A stripped image has no symbol table and nothing to say.  The arm that
# carries this check sets -DAMINETXDUO_KEEP_SYMBOLS=ON.
# grep -c, not grep -q: `-q` exits on the first match and closes the pipe, nm
# takes SIGPIPE, and `set -o pipefail` turns a SUCCESSFUL search into a failed
# pipeline.  This gate reported `stripped` against an image with 1,548 text
# symbols until that was found.
syms=$("$NM" "$LIB" 2>/dev/null | grep -c " T _")
if [ "$syms" -eq 0 ]; then
    echo "hot_calls=skipped reason=stripped"
    exit 0
fi

DIS="$(mktemp)"
trap 'rm -f "$DIS"' EXIT
"$OBJDUMP" -d "$LIB" 2>/dev/null > "$DIS"

lines=$(wc -l < "$DIS")
if [ "$lines" -lt 1000 ]; then
    echo "hot_calls=FAIL reason=no_disassembly lines=$lines"
    exit 1
fi

# The gate is only as good as its ability to SEE a call.  Prove the counter
# works on this image before trusting a zero: a function everyone agrees is
# called out of line must come back non-zero.
canary=$(grep "jsr" "$DIS" | grep -c "_n68k_rx_verify_sum")
if [ "$canary" -eq 0 ]; then
    echo "hot_calls=FAIL reason=canary_not_found sym=_n68k_rx_verify_sum"
    echo "  The jsr counter found no call to a function that is called once"
    echo "  per received frame.  Either the disassembly format changed or the"
    echo "  receive path did; a zero from the list below would be meaningless."
    exit 1
fi

bad=0
checked=0
for sym in $MUST_BE_INLINED; do
    checked=$((checked + 1))
    n=$(grep "jsr" "$DIS" | grep -c "$sym")
    if [ "$n" -ne 0 ]; then
        echo "hot_calls=FAIL sym=$sym jsr_sites=$n"
        echo "  $sym is on the per-frame receive path and used to be inlined."
        echo "  A call there costs about 40-60 cycles a frame at ~480 frames a"
        echo "  second.  Either restore the inlining or, if the call is now"
        echo "  correct, move the name to the out-of-line list with the reason."
        bad=$((bad + 1))
    fi
done

echo "hot_calls_checked=$checked canary=$canary symbols=$syms"
if [ "$bad" -ne 0 ]; then
    echo "hot_calls=FAIL out_of_line=$bad"
    exit 1
fi

echo "hot_calls=PASS inlined=$checked"
exit 0
