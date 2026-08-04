#!/usr/bin/env bash
#
# Everything CI does, in one script, so it can be run before pushing.
#
#   tools/ci.sh                      # tier 1: host tests + every cross config
#   tools/ci.sh host                 # just the host tests
#   tools/ci.sh cross                # just the cross builds
#   tools/ci.sh emulator             # tier 2: FS-UAE, needs a boot ROM
#   tools/ci.sh host cross emulator  # pick and choose
#
# .github/workflows/ci.yml and emulator.yml call THIS, they add caching,
# a matrix and a job summary and nothing else.  If it passes here it passes
# there, and a workflow edit cannot quietly change what is tested.
#
# STAGES
#
#   toolchain    resolve, or download, the pinned m68k-amigaos-gcc
#   host         the parser / mbuf / BPF VM / crypto68k vector tests, ctest
#   host32       the mDNS and TLS-crypto fuzz drivers, which need a 32-bit build
#   cross        every build configuration, warnings fatal
#   analyze      GCC -fanalyzer over our own sources vs a triaged baseline
#   conformance  build the bsdsocktest suite for m68k (running it needs tier 2)
#   emulator     tier 2, boots FS-UAE, needs a ROM
#
# `tools/ci.sh` with no arguments runs toolchain, host, host32, cross, analyze
# and conformance: everything that needs neither an emulator nor a licensed ROM.
#
# ENVIRONMENT
#
#   AMIGA_TOOLCHAIN_ROOT   use this toolchain instead of fetching one
#   AMINETXDUO_CI_BUILD    build directory root (default build/ci)
#   AMINETXDUO_CI_JOBS     parallel jobs (default: all cores)
#   AMINETXDUO_CI_CROSS    space-separated subset of the cross configs to
#                          build, e.g. "default" (default: all of them)
#   AMINETXDUO_KICKSTART   emulator stage: boot ROM.  Required.
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
# TLS and IPv6 are both ON by default now, so `default` covers them and the
# entries here are the OFF ones, the configurations a user gets by asking for
# a smaller stack, and the ones that would otherwise stop being compiled at all.
#
# Then the three CPU targets.  They are not "the same build with a different
# -m flag": each one changes what the compiler may emit and what the tree may
# contain, and each broke something the others did not while it was being
# brought up (docs/RESEARCH.md §45).
#
#   m68000  no 32-bit multiply or divide at all, so the compiler runtime in
#           src/common carries five more routines and the crypto assembly
#           cannot be assembled.  TLS is off by default here.
#   m68040  -m68020 -mtune=68040.  Cheap to build and it is what catches
#           anyone "fixing" that mapping to -m68040, which would silently
#           link the 68000 C library.
#   m68060  the 64-bit-result MULU.L and DIVU.L are gone, so GCC calls
#           __muldi3, the symbol whose absence blocked this target.
#
# `default`, m68000, m68060 and minimal68000 are the four libraries the archive
# ships, in the options the release workflow gives them, so a break in any of
# them is a break in something a user downloads.  Nothing may ship in a shape
# that is not built here.
CROSS_CONFIGS=(
    "default:"
    "noipv6:-DAMINETXDUO_IPV6=OFF"
    # mDNS off is a real code path and not only a smaller library: the browse
    # reaches src/netstack through calls that are not compiled here, and
    # ShowNetServices and the ARexx host's SERVICES both have to build and
    # answer sensibly without them.
    "nomdns:-DAMINETXDUO_MDNS=OFF"
    "notls:-DAMINETXDUO_TLS=OFF"
    # The floor drawer's answer to IGMP. mcast.c is the only caller of NetX
    # Duo's IGMP services, so this arm is what proves the rest of the tree
    # still builds and binds without it, bind() classifies a class D address
    # here and not there.
    "nomcast:-DAMINETXDUO_MULTICAST=OFF"
    "noasm:-DAMINETXDUO_CRYPTO68K_ASM=OFF"
    "m68000:-DAMINETXDUO_CPU=68000"
    "m68040:-DAMINETXDUO_CPU=68040"
    "m68060:-DAMINETXDUO_CPU=68060"
    # The fourth drawer in the archive, and the only arm here that turns more
    # than one thing off at once.  Every option above is a separate arm because
    # each has its own compile-time surface; this one exists because the
    # combination is what a user downloads, and the arms above do not cover it
    # BPF=OFF appears nowhere else at all, and the interactions between five
    # of them appear nowhere else at all.  It must stay byte-for-byte the
    # options .github/workflows/release.yml gives build/release-68000-minimal.
    "minimal68000:-DAMINETXDUO_CPU=68000 -DAMINETXDUO_IPV6=OFF -DAMINETXDUO_MDNS=OFF -DAMINETXDUO_BPF=OFF -DAMINETXDUO_TLS=OFF -DAMINETXDUO_MULTICAST=OFF -DAMINETXDUO_AREXX=OFF -DAMINETXDUO_TCPDEVICE=OFF"
)

# Host-side test executables.  ctest fails loudly ("Unable to find executable")
# if one is registered but not built, so a test added without touching this
# list turns CI red rather than silently disappearing, which is what used to
# happen when `ctest` reported "No tests were found" and nobody noticed.
HOST_TEST_TARGETS=(test_config test_usergroup test_mbuf test_bpf test_httppath test_httpif test_fetchurl test_crypto68k test_crypto68k_25519 test_net68k_checksum
                   test_tcp_retries test_bcast_loopback test_tcp_source_connect test_tcp_rtt
                   test_dns_retry test_dns_status
                   test_sockopt_numbers test_sana2_copy test_ipv6_ra test_ipv6_ptb
                   test_httpframe test_tls_x509 test_ipv6_frag
                   fuzz_config fuzz_bpf fuzz_dns fuzz_usergroup
                   fuzz_dhcp fuzz_tls_record fuzz_tls_x509 fuzz_httpframe)

# The on-Amiga harnesses this stage runs.  Verified 2026-07-25 against
# Kickstart 3.1, identical check counts on both.  Deliberately NOT here:
#   netstack_test, and the bsdsocktest conformance suite, both of which need
#   DEVS:a2065.device, Commodore's driver, not redistributable, so those
#   stay on a machine that has one (see tools/ci.sh emulator + a real ROM).
#
# tools/smoke/crashtest and tools/smoke/gurutest are excluded on purpose: they
# fault deliberately, so a nonzero exit is their success condition and this
# loop would read it as a failure.
EMULATOR_TESTS=(
    "tests/bracket/bracket_invariants:150"
    "tools/smoke/smoke:90"
    "tools/smoke/lifecycle:120"
    "tools/smoke/KernelStop:150"
    # The 68000 arm below is the whole point of this one: it is the only machine
    # in the matrix that raises an Address Error rather than tolerating an
    # unaligned word, so it is the only one where the alignment fixes can fail.
    "tools/smoke/alignprobe:90"
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
        note "no toolchain on this machine, fetching the pinned one"
        eval "$(tools/fetch-toolchain.sh --export)"
    fi
    . tools/amiga-toolchain.sh
    export AMIGA_TOOLCHAIN_ROOT

    # A locally installed toolchain is not the toolchain CI builds with, and the
    # difference is not only the compiler: NDK header sets diverge.  A green
    # cross build here against an NDK that declares SetRexxVarFromMsg said
    # nothing about CI, whose pinned NDK does not, which is how v0.13.0 got
    # tagged on code that did not compile.  Warn rather than override: building
    # against what you have installed is usually what you want locally.
    # Resolved with pwd -P, because the pinned tree is normally reached through
    # a `current` symlink: comparing the strings reports the pinned toolchain as
    # not pinned, and a warning that fires when nothing is wrong gets ignored.
    local pinned have
    pinned=$(tools/fetch-toolchain.sh --print-root 2>/dev/null || true)
    [ -d "$pinned" ] && pinned=$(cd "$pinned" && pwd -P)
    have=$(cd "$AMIGA_TOOLCHAIN_ROOT" && pwd -P)
    if [ -n "$pinned" ] && [ "$have" != "$pinned" ]; then
        note "NOT the pinned toolchain CI uses ($pinned)"
        note "  NDK header sets differ, a green build here can still fail CI"
    fi
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

# --------------------------------------------------------------- host32 ----

# The two fuzz drivers that need a 32-bit host, for different reasons.
#
#   fuzz_mdns         NetX Duo's mDNS cache keeps pointers in ULONG slots, so
#                     it is only coherent where sizeof(void*) == 4.
#   fuzz_tls_crypto   ami_tls_crypto.c and
#                     nx_secure_tls_process_certificate_verify.c both cast a
#                     pointer to a 32-bit ULONG, and in the second one that
#                     cast is the signature bounds check under test.
#
# Both read bytes chosen by someone else, unauthenticated multicast, and a
# TLS server's handshake before any key exists to check it against, which
# makes them worth a build of their own rather than leaving them unexercised.
HOST32_TEST_TARGETS=(fuzz_mdns fuzz_tls_crypto)

stage_host32() {
    hr "host tests (32-bit: mDNS + TLS crypto fuzz)"

    if ! (echo 'int main(void){return 0;}' > "$BUILD/m32probe.c" &&
          "${CC:-cc}" -m32 "$BUILD/m32probe.c" -o "$BUILD/m32probe") 2>/dev/null; then
        note "no -m32 on this host (needs gcc-multilib on Debian/Ubuntu), skipped"
        return 0
    fi

    cmake -S . -B "$BUILD/host32" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_FLAGS=-m32 -DCMAKE_CXX_FLAGS=-m32 -DCMAKE_EXE_LINKER_FLAGS=-m32 \
        > "$BUILD/host32-configure.log" 2>&1 || {
            tail -30 "$BUILD/host32-configure.log"; fail "host32 configure"; return 1; }

    cmake --build "$BUILD/host32" --parallel "$JOBS" \
        --target "${HOST32_TEST_TARGETS[@]}" \
        || { fail "host32 build"; return 1; }

    ( cd "$BUILD/host32" && ctest --output-on-failure -R 'mdns|tls_crypto' ) \
        || { fail "host32 ctest"; return 1; }

    # A 64-bit build registers none of these at all, so an empty run here would
    # otherwise pass as a green stage that tested nothing.  Two per driver.
    local n want
    want=$(( ${#HOST32_TEST_TARGETS[@]} * 2 ))
    n=$( (cd "$BUILD/host32" && ctest -N -R 'mdns|tls_crypto' 2>/dev/null | sed -n 's/^Total Tests: //p') )
    note "$n 32-bit fuzz test(s) registered"
    [ "${n:-0}" -ge "$want" ] || {
        fail "host32 registered $n tests, expected $want"; return 1; }
}

# ----------------------------------------------------------------- cross ----

stage_cross() {
    local entry name opts

    # The Developer drawer's inline/proto/pragma headers are committed, so
    # packaging never needs sfdc, which means nothing would notice them
    # drifting from the SFD they came from.  This is what notices.
    if [ -x "${AMIGA_TOOLCHAIN_ROOT:-}/bin/sfdc" ] || command -v sfdc >/dev/null; then
        if tools/gen-developer.sh --check > "$BUILD/gen-developer.log" 2>&1; then
            note "Developer drawer headers match developer/sfd/aminetxduo_lib.sfd"
        else
            cat "$BUILD/gen-developer.log"
            fail "Developer drawer headers are stale (tools/gen-developer.sh)"
        fi
    else
        note "no sfdc, Developer drawer headers NOT checked against their SFD"
    fi

    for entry in "${CROSS_CONFIGS[@]}"; do
        name="${entry%%:*}"
        opts="${entry#*:}"

        # AMINETXDUO_CI_CROSS=default builds just one, what the emulator
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
            # `|| tail`, not a bare pipeline.  Under `set -euo pipefail` the
            # grep exits 1 when nothing matches and 141 when head -20 closes
            # the pipe on a broad break, and either one killed the shell here
            # before `fail` recorded anything, before the remaining configs
            # were attempted and before the summary printed.  The fallback also
            # gives this path the log tail the configure path above already has,
            # so a failure is never reported with no output at all.
            grep -E "error:|Error" "$BUILD/$name-build.log" | head -20 \
                || tail -30 "$BUILD/$name-build.log"
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
        note "third_party/bsdsocktest not checked out, skipping"
        return 0
    fi
    tests/conformance/build.sh > "$BUILD/conformance.log" 2>&1 || {
        tail -30 "$BUILD/conformance.log"; fail "conformance build"; return 1; }
    note "$(grep -c '  CC ' "$BUILD/conformance.log" || true) translation units"
    note "build/bsdsocktest/bsdsocktest"
}

# -------------------------------------------------------------- analyse ----

stage_analyze() {
    #
    # Off unless AMINETXDUO_ANALYZE=1. It is 2.5 minutes, its findings have not
    # moved in weeks, and it was being run over and over by parallel agents who
    # had been told to "run the full set once at the end", which is the wrong
    # instruction and was mine. A refusal here holds whatever a brief says; the
    # release workflow sets the variable, so nothing ships unanalysed.
    #
    if [ "${AMINETXDUO_ANALYZE:-0}" != "1" ]; then
        hr "static analysis (cross)"
        note "SKIPPED, set AMINETXDUO_ANALYZE=1 to run it (the release does)"
        return 0
    fi

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
        note "cppcheck NOT INSTALLED, that half of this stage did not run"
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
        fail "fs-uae is not installed, tier 2 cannot run"
        return 1
    fi

    if [ -n "${AMINETXDUO_KICKSTART:-}" ]; then
        note "boot ROM: $AMINETXDUO_KICKSTART (supplied)"
    else
        fail "no AMINETXDUO_KICKSTART, this stage needs a boot ROM"
        return 1
    fi
    export AMINETXDUO_KICKSTART
    export AMINETXDUO_KICKSTART_EXT="${AMINETXDUO_KICKSTART_EXT:-}"

    # TWO MACHINES, not one.  The default cross build on a 68020, and the
    # m68000 build on an actual 68000, which is a different compiler output
    # (no 32-bit multiply or divide, the 68000 C library) executing under a
    # different scheduler budget, and for a long time nothing ran it at all.
    #
    # That gap hid a real defect in this suite: soak_test's starvation floor
    # was a constant tuned against 68020 throughput, so the lowest-priority
    # worker failed it on a 68000 while being perfectly healthy.  A build that
    # is never executed is not tested, and we ship a 68000 library.
    for dir in default m68000; do
        if [ ! -d "$BUILD/$dir" ]; then
            fail "no $BUILD/$dir, run the cross stage first"
            return 1
        fi
    done

    local entry exe timeout dir cpuopt tag budget
    for dir in default m68000; do
        # These budgets multiply the per-test ceilings below.  They are ceilings,
        # not fixed waits, so a passing run finishes long before them and paying
        # for headroom is free, and the hosted GitHub runner is markedly slower
        # at emulation than a dedicated box, enough to time a test out on the old
        # 68020 x1 / 68000 x2 budgets.  Doubled so a slow runner has rope.
        if [ "$dir" = "m68000" ]; then
            cpuopt="-c 68000"; tag="68000"; budget=4
        else
            cpuopt="";         tag="68020"; budget=2
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

#
# analyze is NOT in the default set. It is 2.5 minutes of the roughly four a
# default run takes, and its findings do not move between commits the way a
# build break does, the baseline has sat at 13 for weeks. Naming it runs it:
#
#     tools/ci.sh analyze                  just it
#     tools/ci.sh host host32 cross analyze conformance    the release set
#
# The release workflow names it, so nothing ships unanalysed. A default run
# says out loud that it skipped, because a stage that goes quiet reads as
# coverage it is not providing.
#
WANT=("$@")
[ ${#WANT[@]} -gt 0 ] || WANT=(host host32 cross conformance)

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
        host32)      stage_host32 || true ;;
        # `|| true` on every stage, cross included: bash suppresses `set -e`
        # inside a function called that way, which is what keeps one unguarded
        # command in a stage from taking the summary down with it.
        cross)       stage_cross || true ;;
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

case " ${STAGES_RUN[*]} " in
    *" analyze "*) ;;
    *) note "analyze NOT RUN, AMINETXDUO_ANALYZE=1 tools/ci.sh analyze" ;;
esac
if [ ${#FAILED[@]} -eq 0 ]; then
    printf '\033[32mall green\033[0m\n'
    exit 0
fi
printf '\033[31m%d failure(s):\033[0m\n' "${#FAILED[@]}"
printf '  - %s\n' "${FAILED[@]}"
exit 1
