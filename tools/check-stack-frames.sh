#!/usr/bin/env bash
#
# No path in the shipping libraries may outgrow the stack it runs on.
#
#   tools/check-stack-frames.sh [build-dir]
#
# A bsdsocket.library vector runs on the CALLER's stack, an AmigaDOS Shell
# hands a command 4096 bytes, and there is no MMU: a frame that does not fit
# does not fault, it writes over what lies below and the machine dies later,
# somewhere unrelated.  tests/stack/stack_test.c measures what an ordinary API
# path actually touches, in a guest, on a real Shell stack.  This is the other
# half: the worst case the compiler will admit to, on every path, without a
# guest and without an event having to happen while the measurement is running.
#
# It exists because the DHCP/RA absorb was the deepest thing in
# bsdsocket.library and nothing measured it.  It only runs when a lease or an
# advertisement is pending, so stack_test could execute an entire clean run
# beside it and report a comfortable margin, and the frames were free to grow
# -- 1280 bytes when they were written down, 1412 by the time anyone measured.
#
# HOW IT WORKS
#
# -fstack-usage gives one frame size per function, -save-temps=obj keeps the
# LTRANS assembly the frame sizes belong to, and tools/stack-depth.py walks the
# jsr/bsr graph over the two.  The build is its own: the flags change no code,
# but they are not in the shipping configuration and this must not depend on a
# build somebody else configured.  Pass a build directory that already has
# .su/.s files in it to skip the build.
#
# THE NUMBERS ARE FLOORS.  A call through a function pointer in a struct is not
# a symbol in the assembly.  The resolver ladder is four such calls and they
# are supplied by hand in LADDER_EDGES below; anything else missing counts as
# zero.  So a budget here is checked against stack_test's measurement, not
# instead of it.
#
# Output is key=value and an exit code: 0 within budget, 1 over, 2 nothing
# checked (no toolchain, no build).
#
# SPDX-License-Identifier: MIT

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${1:-$ROOT/build/stackframes}"
JOBS="${AMINETXDUO_CI_JOBS:-$( (command -v nproc >/dev/null && nproc) || echo 4 )}"

# The resolver ladder calls its ask function through AmiNetAskFn, so the graph
# has a hole exactly where the deepest paths are.  These close it.
LADDER_EDGES=(
    --edge ami_net_ask_until=ami_ns_ask_name
    --edge ami_net_ask_until=ami_ns_ask_addr
    --edge ami_net_ask_until=ami_ns_ask_name6
    --edge ami_net_ask_until=ami_ns_ask_mdns
    --edge ami_ns_resolve_once=ami_net_ask_until
    --edge ami_ns_resolve6_once=ami_net_ask_until
    --edge netstack_resolve_reverse_until=ami_net_ask_until
    --edge netstack_resolve_until=ami_ns_resolve_once
)

# WHAT IS CHECKED, and what each number is for.
#
#   binary  root symbol                     budget  measured  what it bounds
#
# bsdsocket.library runs on a Shell's 4096 with the calling program's own
# frames already in it.  stack_test measures 2840 touched for the same calls,
# so the static figure runs about 800 bytes light; a budget of 2432 against
# 2016 measured leaves the guest number under 3700 with the eighth of a Shell
# stack stack_test insists on still spare.
#
# The absorb has its own row because it is the one that regressed unobserved,
# and because it is bounded by construction now: its buffers are in an
# AllocMem block, so anything that puts one back on the stack shows up here as
# hundreds of bytes and not as tens.
#
# tls.library is not on a Shell stack -- every program that opens it must
# StackSwap first, and src/tools/fetch.c asks for 64 KB -- so its budget is
# about noticing growth, not about survival.
BUDGETS=(
    "bsdsocket:bsd_getaddrinfo:2432"
    "bsdsocket:bsd_getnameinfo:2432"
    "bsdsocket:bsd_gethostbyname:2432"
    "bsdsocket:bsd_gethostbyaddr:2432"
    "bsdsocket:ami_ns_dns_absorb_pending:768"
    "tlslib:ami_crypto_method_rsa_operation:3456"
    "tlslib:_nx_crypto_huge_number_modulus:2688"
)

say() { echo "$@"; }

# ------------------------------------------------------------------ build ---

# Both halves, or the graph has frame sizes and no edges and every root looks
# like a leaf -- which would pass every budget while measuring nothing.
need_build=1
if [ -d "$BUILD" ] &&
   [ -n "$(find "$BUILD" -name '*.su' -print -quit 2>/dev/null)" ] &&
   [ -n "$(find "$BUILD" -name '*.ltrans*.s' -print -quit 2>/dev/null)" ]; then
    need_build=0
fi

if [ "$need_build" = 1 ]; then
    # shellcheck disable=SC1091
    . "$ROOT/tools/amiga-toolchain.sh" >/dev/null 2>&1 || true
    if [ -z "${AMIGA_TOOLCHAIN_ROOT:-}" ] ||
       [ ! -x "${AMIGA_TOOLCHAIN_ROOT}/bin/m68k-amigaos-gcc" ]; then
        say "stack_frames=skipped reason=no_toolchain"
        exit 2
    fi

    if ! cmake -S "$ROOT" -B "$BUILD" \
            -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-m68k-amigaos.cmake \
            -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_C_FLAGS="-fstack-usage -save-temps=obj" \
            > "$BUILD-configure.log" 2>&1; then
        say "stack_frames=skipped reason=configure_failed log=$BUILD-configure.log"
        exit 2
    fi

    if ! cmake --build "$BUILD" --parallel "$JOBS" \
            --target bsdsocket_library tls_library \
            > "$BUILD-build.log" 2>&1; then
        say "stack_frames=skipped reason=build_failed log=$BUILD-build.log"
        exit 2
    fi
fi

DEPTH="$ROOT/tools/stack-depth.py"
PYTHON="$(command -v python3 || command -v python || true)"
if [ -z "$PYTHON" ] || [ ! -f "$DEPTH" ]; then
    say "stack_frames=skipped reason=no_python"
    exit 2
fi

# ---------------------------------------------------------------- measure ---

rc=0
checked=0
for row in "${BUDGETS[@]}"; do
    IFS=: read -r lib sym budget <<< "$row"

    dir="$BUILD/src/$lib"
    if [ ! -d "$dir" ]; then
        say "stack_frames=skipped reason=no_objects binary=$lib"
        exit 2
    fi

    out=$("$PYTHON" "$DEPTH" --quiet "${LADDER_EDGES[@]}" "$dir" "$sym" 2>/dev/null)
    got=${out##*worst_case_bytes=}
    got=${got%%[!0-9]*}

    if [ -z "$got" ] || [ "$got" = 0 ]; then
        # A root that resolves to nothing is a broken check, not a pass: it is
        # how a renamed symbol would quietly stop being measured.
        say "stack_frames=FAILED binary=$lib root=$sym reason=symbol_not_found"
        rc=1
        continue
    fi

    checked=$((checked + 1))
    if [ "$got" -gt "$budget" ]; then
        say "stack_frames=FAILED binary=$lib root=$sym bytes=$got budget=$budget"
        say "  the deepest path:"
        "$PYTHON" "$DEPTH" "${LADDER_EDGES[@]}" "$dir" "$sym" 2>/dev/null |
            sed -n '2,$p' | sed 's/^/  /'
        rc=1
    else
        say "stack_frames_bytes binary=$lib root=$sym bytes=$got budget=$budget"
    fi
done

if [ "$rc" = 0 ]; then
    say "stack_frames=clean roots=$checked"
fi
exit "$rc"
