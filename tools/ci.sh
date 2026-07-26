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
#   cross        all four build configurations, warnings fatal
#   conformance  build the bsdsocktest suite for m68k (running it needs tier 2)
#   emulator     tier 2 -- boots FS-UAE, needs a ROM
#
# `tools/ci.sh` with no arguments runs toolchain, host, cross and conformance:
# everything that needs neither an emulator nor a licensed ROM.
#
# ENVIRONMENT
#
#   AMIGA_TOOLCHAIN_ROOT   use this toolchain instead of fetching one
#   AMINETXDUO_CI_BUILD    build directory root (default build/ci)
#   AMINETXDUO_CI_JOBS     parallel jobs (default: all cores)
#   AMINETXDUO_CI_CROSS    space-separated subset of the cross configs to
#                          build, e.g. "default" (default: all four)
#   AMINETXDUO_KICKSTART   emulator stage: boot ROM.  Unset means fetch AROS.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"

BUILD="${AMINETXDUO_CI_BUILD:-build/ci}"
JOBS="${AMINETXDUO_CI_JOBS:-$( (command -v nproc >/dev/null && nproc) || sysctl -n hw.ncpu 2>/dev/null || echo 4 )}"

# The four configurations that must all build.  They are not variations on a
# theme: AMINETXDUO_IPV6 changes the layout of NX_IP, NX_PACKET and
# NX_TCP_SOCKET across the whole tree, AMINETXDUO_TLS pulls in nx_secure and
# nx_crypto, and AMINETXDUO_CRYPTO68K_ASM=OFF swaps the hand-written 68020
# limb primitives for the portable C.  Each has broken while the others built.
#
# TLS is ON by default now, so `default` covers the TLS build and the entry
# here is the OFF one -- the configuration a user gets by asking for a smaller
# stack, and the one that would otherwise stop being compiled at all.
CROSS_CONFIGS=(
    "default:"
    "ipv6:-DAMINETXDUO_IPV6=ON"
    "notls:-DAMINETXDUO_TLS=OFF"
    "noasm:-DAMINETXDUO_CRYPTO68K_ASM=OFF"
)

# Host-side test executables.  ctest fails loudly ("Unable to find executable")
# if one is registered but not built, so a test added without touching this
# list turns CI red rather than silently disappearing -- which is what used to
# happen when `ctest` reported "No tests were found" and nobody noticed.
HOST_TEST_TARGETS=(test_config test_mbuf test_bpf test_crypto68k test_crypto68k_25519 test_net68k_checksum)

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

    # The emulator tier runs the DEFAULT cross build; stage_cross has to have
    # produced it.
    if [ ! -d "$BUILD/default" ]; then
        fail "no $BUILD/default -- run the cross stage first"
        return 1
    fi

    local entry exe timeout
    for entry in "${EMULATOR_TESTS[@]}"; do
        exe="${entry%%:*}"
        timeout="${entry##*:}"
        printf '\n-- %s\n' "$exe"
        if [ ! -f "$BUILD/default/$exe" ]; then
            fail "emulator: $exe was not built"
            continue
        fi
        if AMINETXDUO_RUN_TAG=ci tools/fsuae-run.sh -t "$timeout" \
               "$BUILD/default/$exe" > "$BUILD/emu-$(basename "$exe").log" 2>&1; then
            note "PASS  $(grep -E '[0-9]+ checks' "$BUILD/emu-$(basename "$exe").log" | tail -1)"
        else
            tail -25 "$BUILD/emu-$(basename "$exe").log"
            fail "emulator: $exe"
        fi
    done
}

# ------------------------------------------------------------------ main ----

mkdir -p "$BUILD"

WANT=("$@")
[ ${#WANT[@]} -gt 0 ] || WANT=(host cross conformance)

stage_submodules

# Anything but a pure host run needs the cross compiler.
for s in "${WANT[@]}"; do
    case "$s" in
        cross|conformance|emulator) stage_toolchain; break ;;
    esac
done

for s in "${WANT[@]}"; do
    case "$s" in
        toolchain)   [ -n "${AMIGA_TOOLCHAIN_ROOT:-}" ] || stage_toolchain ;;
        host)        stage_host || true ;;
        cross)       stage_cross ;;
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
