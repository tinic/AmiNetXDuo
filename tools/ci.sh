#!/usr/bin/env bash
#
# Everything CI does, in one script, so it can be run before pushing.
#
#   tools/ci.sh                      # tier 1: host tests + every cross config
#   tools/ci.sh host                 # just the host tests
#   tools/ci.sh cross                # just the cross builds
#   tools/ci.sh emulator             # tier 2: FS-UAE on the AROS ROM
#   tools/ci.sh host cross emulator  # pick and choose
#
# .github/workflows/ci.yml and emulator.yml call THIS -- they add caching,
# a matrix and a job summary and nothing else.  If it passes here it passes
# there, and a workflow edit cannot quietly change what is tested.
#
# STAGES
#
#   toolchain    resolve, or download, the pinned m68k-amigaos-gcc
#   host         the parser / mbuf / BPF VM / crypto68k vector tests, ctest
#   cross        every build configuration, warnings fatal
#   analyze      GCC -fanalyzer over our own sources vs a triaged baseline
#   conformance  build the bsdsocktest suite for m68k (running it needs tier 2)
#   emulator     tier 2 -- boots FS-UAE, needs a ROM
#
# `tools/ci.sh` with no arguments runs toolchain, host, cross, analyze and
# conformance: everything that needs neither an emulator nor a licensed ROM.
#
# ENVIRONMENT
#
#   AMIGA_TOOLCHAIN_ROOT   use this toolchain instead of fetching one
#   AMINETXDUO_CI_BUILD    build directory root (default build/ci)
#   AMINETXDUO_CI_JOBS     parallel jobs (default: all cores)
#   AMINETXDUO_CI_CROSS    space-separated subset of the cross configs to
#                          build, e.g. "default" (default: all of them)
#   AMINETXDUO_KICKSTART   emulator stage: boot ROM.  Unset means fetch AROS.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"

BUILD="${AMINETXDUO_CI_BUILD:-build/ci}"
JOBS="${AMINETXDUO_CI_JOBS:-$( (command -v nproc >/dev/null && nproc) || sysctl -n hw.ncpu 2>/dev/null || echo 4 )}"

# The configurations that must all build.  They are not variations on a
# theme: AMINETXDUO_IPV6 changes the layout of NX_IP, NX_PACKET and
# NX_TCP_SOCKET across the whole tree, AMINETXDUO_TLS pulls in nx_secure and
# nx_crypto, and AMINETXDUO_CRYPTO68K_ASM=OFF swaps the hand-written 68020
# limb primitives for the portable C.  Each has broken while the others built.
#
# TLS is ON by default now, so `default` covers the TLS build and the entry
# here is the OFF one -- the configuration a user gets by asking for a smaller
# stack, and the one that would otherwise stop being compiled at all.
#
# The last three are the CPU targets.  They are not "the same build with a
# different -m flag": each one changes what the compiler may emit and what the
# tree may contain, and each broke something the others did not while it was
# being brought up (docs/RESEARCH.md §45).
#
#   m68000  no 32-bit multiply or divide at all, so the compiler runtime in
#           src/common carries five more routines and the crypto assembly
#           cannot be assembled.  TLS is off by default here.
#   m68040  -m68020 -mtune=68040.  Cheap to build and it is what catches
#           anyone "fixing" that mapping to -m68040, which would silently
#           link the 68000 C library.
#   m68060  the 64-bit-result MULU.L and DIVU.L are gone, so GCC calls
#           __muldi3 -- the symbol whose absence blocked this target.
#
# Together with `default` these are the three libraries the archive ships, so
# a break here is a break in something a user downloads.
CROSS_CONFIGS=(
    "default:"
    "ipv6:-DAMINETXDUO_IPV6=ON"
    "notls:-DAMINETXDUO_TLS=OFF"
    "noasm:-DAMINETXDUO_CRYPTO68K_ASM=OFF"
    "m68000:-DAMINETXDUO_CPU=68000"
    "m68040:-DAMINETXDUO_CPU=68040"
    "m68060:-DAMINETXDUO_CPU=68060"
)

# Host-side test executables.  ctest fails loudly ("Unable to find executable")
# if one is registered but not built, so a test added without touching this
# list turns CI red rather than silently disappearing -- which is what used to
# happen when `ctest` reported "No tests were found" and nobody noticed.
HOST_TEST_TARGETS=(test_config test_mbuf test_bpf test_crypto68k test_crypto68k_25519 test_net68k_checksum
                   test_tcp_retries fuzz_config fuzz_bpf)

# The on-Amiga harnesses the AROS ROM can run.  Verified 2026-07-25 against
# Kickstart 3.1 -- identical check counts on both.  Deliberately NOT here:
#   netstack_test, and the bsdsocktest conformance suite, both of which need
#   DEVS:a2065.device -- Commodore's driver, not redistributable, so those
#   stay on a machine that has one (see tools/ci.sh emulator + a real ROM).
#
# tools/smoke/crashtest and tools/smoke/gurutest are excluded on purpose: they
# fault deliberately, so a nonzero exit is their success condition and this
# loop would read it as a failure.
EMULATOR_TESTS=(
    "tools/smoke/smoke:90"
    "tools/smoke/lifecycle:120"
    "tools/smoke/KernelStop:150"
    "tests/ram_driver/ram_driver_test:120"
    "tests/mbuf_bpf/mbuf_bpf_test:180"
    "tests/soak/soak_test:240"
)

FAILED=()
STAGES_RUN=()

hr()   { printf '\n\033[1m======== %s\033[0m\n' "$*"; }
note() { printf '  %s\n' "$*"; }
fail() { FAILED+=("$1"); printf '\033[31m!! FAILED: %s\033[0m\n' "$1" >&2; }

# ------------------------------------------------------------ submodules ----

stage_submodules() {
    if [ ! -f third_party/threadx/common/inc/tx_api.h ] ||
       [ ! -f third_party/netxduo/common/inc/nx_api.h ]; then
        hr "submodules"
        git submodule update --init --recursive
    fi
}

# ------------------------------------------------------------- toolchain ----

stage_toolchain() {
    hr "toolchain"
    if [ -n "${AMIGA_TOOLCHAIN_ROOT:-}" ] && [ -x "$AMIGA_TOOLCHAIN_ROOT/bin/m68k-amigaos-gcc" ]; then
        note "using AMIGA_TOOLCHAIN_ROOT=$AMIGA_TOOLCHAIN_ROOT"
    elif AMIGA_TOOLCHAIN_QUIET=1 . tools/amiga-toolchain.sh 2>/dev/null; then
        note "found $AMIGA_TOOLCHAIN_ROOT"
    else
        note "no toolchain on this machine -- fetching the pinned one"
        eval "$(tools/fetch-toolchain.sh --export)"
    fi
    . tools/amiga-toolchain.sh
    export AMIGA_TOOLCHAIN_ROOT
}

# ------------------------------------------------------------------ host ----

stage_host() {
    hr "host tests"

    cmake -S . -B "$BUILD/host" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_PROJECT_INCLUDE="$ROOT/cmake/ci-warnings.cmake" \
        > "$BUILD/host-configure.log" 2>&1 || {
            tail -30 "$BUILD/host-configure.log"; fail "host configure"; return 1; }

    # Not `--target all`: most of the tree is AmigaOS code that has no meaning
    # on the host, so only the test executables are asked for.
    cmake --build "$BUILD/host" --parallel "$JOBS" \
        --target "${HOST_TEST_TARGETS[@]}" || { fail "host build"; return 1; }

    ( cd "$BUILD/host" && ctest --output-on-failure ) || { fail "ctest"; return 1; }

    local n
    n=$( (cd "$BUILD/host" && ctest -N 2>/dev/null | sed -n 's/^Total Tests: //p') )
    note "$n tests registered"
    if [ "${n:-0}" -lt "${#HOST_TEST_TARGETS[@]}" ]; then
        fail "only $n tests registered, expected at least ${#HOST_TEST_TARGETS[@]}"
        return 1
    fi
}

# ----------------------------------------------------------------- cross ----

stage_cross() {
    local entry name opts
    for entry in "${CROSS_CONFIGS[@]}"; do
        name="${entry%%:*}"
        opts="${entry#*:}"

        # AMINETXDUO_CI_CROSS=default builds just one -- what the emulator
        # tier needs, and what you want when bisecting.
        if [ -n "${AMINETXDUO_CI_CROSS:-}" ]; then
            case " $AMINETXDUO_CI_CROSS " in
                *" $name "*) ;;
                *) continue ;;
            esac
        fi

        hr "cross build: $name ${opts:-(default)}"

        # shellcheck disable=SC2086
        cmake -S . -B "$BUILD/$name" \
            -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-m68k-amigaos.cmake \
            -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_PROJECT_INCLUDE="$ROOT/cmake/ci-warnings.cmake" \
            $opts > "$BUILD/$name-configure.log" 2>&1 || {
                tail -30 "$BUILD/$name-configure.log"; fail "configure $name"; continue; }

        if cmake --build "$BUILD/$name" --parallel "$JOBS" > "$BUILD/$name-build.log" 2>&1; then
            note "built clean"
        else
            grep -E "error:|Error" "$BUILD/$name-build.log" | head -20
            fail "build $name"
        fi
    done
}

# ----------------------------------------------------------- conformance ----

stage_conformance() {
    hr "conformance suite (build only)"
    # It is a submodule and it compiles for m68k, so it belongs in the build
    # tier even though running it needs an emulator AND a2065.device.
    if [ ! -e third_party/bsdsocktest/.git ]; then
        note "third_party/bsdsocktest not checked out -- skipping"
        return 0
    fi
    tests/conformance/build.sh > "$BUILD/conformance.log" 2>&1 || {
        tail -30 "$BUILD/conformance.log"; fail "conformance build"; return 1; }
    note "$(grep -c '  CC ' "$BUILD/conformance.log" || true) translation units"
    note "build/bsdsocktest/bsdsocktest"
}

# -------------------------------------------------------------- analyse ----

stage_analyze() {
    hr "static analysis (cross)"

    export AMINETXDUO_ANALYZE_BUILD="$BUILD/analyze"

    if tools/analyze.sh > "$BUILD/analyze.log" 2>&1; then
        # The uncovered units are the number worth reading, so print them here
        # too rather than only in the log nobody opens on a green run.
        grep '^NOT COVERED' "$BUILD/analyze.log" | while read -r l; do note "$l"; done
        note "$(grep 'known findings' "$BUILD/analyze.log")"
    else
        cat "$BUILD/analyze.log"
        fail "analyze (-fanalyzer)"
    fi

    # cppcheck is not part of the toolchain and CI runners may not have it.
    # Say so out loud rather than passing quietly: a stage that skips without
    # a word reads as coverage it is not providing.
    if ! command -v cppcheck > /dev/null; then
        note "cppcheck NOT INSTALLED -- that half of this stage did not run"
        return 0
    fi
    if tools/cppcheck.sh > "$BUILD/cppcheck.log" 2>&1; then
        note "$(grep 'known findings' "$BUILD/cppcheck.log")"
    else
        cat "$BUILD/cppcheck.log"
        fail "analyze (cppcheck)"
    fi
}

# -------------------------------------------------------------- emulator ----

stage_emulator() {
    hr "emulator tests (tier 2)"

    if ! command -v fs-uae >/dev/null 2>&1 && [ -z "${FSUAE:-}" ]; then
        fail "fs-uae is not installed -- tier 2 cannot run"
        return 1
    fi

    if [ -n "${AMINETXDUO_KICKSTART:-}" ]; then
        note "boot ROM: $AMINETXDUO_KICKSTART (supplied)"
    else
        note "no AMINETXDUO_KICKSTART -- fetching the AROS m68k ROM"
        eval "$(tools/fetch-aros-rom.sh --export)"
    fi
    export AMINETXDUO_KICKSTART
    export AMINETXDUO_KICKSTART_EXT="${AMINETXDUO_KICKSTART_EXT:-}"

    # TWO MACHINES, not one.  The default cross build on a 68020, and the
    # m68000 build on an actual 68000 -- which is a different compiler output
    # (no 32-bit multiply or divide, the 68000 C library) executing under a
    # different scheduler budget, and for a long time nothing ran it at all.
    #
    # That gap hid a real defect in this suite: soak_test's starvation floor
    # was a constant tuned against 68020 throughput, so the lowest-priority
    # worker failed it on a 68000 while being perfectly healthy.  A build that
    # is never executed is not tested, and we ship a 68000 library.
    for dir in default m68000; do
        if [ ! -d "$BUILD/$dir" ]; then
            fail "no $BUILD/$dir -- run the cross stage first"
            return 1
        fi
    done

    local entry exe timeout dir cpuopt tag budget
    for dir in default m68000; do
        if [ "$dir" = "m68000" ]; then
            cpuopt="-c 68000"; tag="68000"; budget=2
        else
            cpuopt="";         tag="68020"; budget=1
        fi

        printf '\n\033[1m-- emulator: %s\033[0m\n' "$tag"

        for entry in "${EMULATOR_TESTS[@]}"; do
            exe="${entry%%:*}"
            timeout="${entry##*:}"
            # A 68000 is roughly a quarter of the 68020 here, so the same work
            # needs a longer rope before a timeout means anything.
            timeout=$(( timeout * budget ))
            printf '\n-- %s (%s)\n' "$exe" "$tag"
            if [ ! -f "$BUILD/$dir/$exe" ]; then
                fail "emulator/$tag: $exe was not built"
                continue
            fi
            local log
            log="$BUILD/emu-$tag-$(basename "$exe").log"
            if AMINETXDUO_RUN_TAG="ci-$tag" tools/fsuae-run.sh $cpuopt \
                   -t "$timeout" "$BUILD/$dir/$exe" > "$log" 2>&1; then
                note "PASS  $(grep -E '[0-9]+ checks' "$log" | tail -1)"
            else
                tail -25 "$log"
                fail "emulator/$tag: $exe"
            fi
        done
    done
}

# ------------------------------------------------------------------ main ----

mkdir -p "$BUILD"

WANT=("$@")
[ ${#WANT[@]} -gt 0 ] || WANT=(host cross analyze conformance)

stage_submodules

# Anything but a pure host run needs the cross compiler.
for s in "${WANT[@]}"; do
    case "$s" in
        cross|analyze|conformance|emulator) stage_toolchain; break ;;
    esac
done

for s in "${WANT[@]}"; do
    case "$s" in
        toolchain)   [ -n "${AMIGA_TOOLCHAIN_ROOT:-}" ] || stage_toolchain ;;
        host)        stage_host || true ;;
        cross)       stage_cross ;;
        analyze)     stage_analyze || true ;;
        conformance) stage_conformance || true ;;
        emulator)    stage_emulator || true ;;
        *) echo "unknown stage: $s" >&2; exit 2 ;;
    esac
    STAGES_RUN+=("$s")
done

hr "summary"
# What was built, in the same words CI names its artefacts with.  This also
# re-checks the recorded NetX Duo/ThreadX versions against third_party/.
note "version: $(tools/version.sh --long 2>&1 || echo 'version.sh FAILED -- see above')"
note "stages: ${STAGES_RUN[*]}"
if [ ${#FAILED[@]} -eq 0 ]; then
    printf '\033[32mall green\033[0m\n'
    exit 0
fi
printf '\033[31m%d failure(s):\033[0m\n' "${#FAILED[@]}"
printf '  - %s\n' "${FAILED[@]}"
exit 1
