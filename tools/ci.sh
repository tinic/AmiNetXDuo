#!/usr/bin/env bash
#
# Everything CI does, in one script, so it can be run before pushing.
#
#   tools/ci.sh                      # tier 1: host tests + every cross config
#   tools/ci.sh host                 # just the host tests
#   tools/ci.sh cross                # just the cross builds
#   tools/ci.sh emulator             # tier 2: FS-UAE, needs a boot ROM
#   tools/ci.sh cards                # tier 2: every network card, one boot each
#   tools/ci.sh bridged lossgate smb # tier 2: the arms that need a real link
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
#   web          httpd's terminal page still matches the TypeScript it is
#                generated from, and the vendored xterm.js is untouched
#   analyze      GCC -fanalyzer over our own sources vs a triaged baseline
#   conformance  build the bsdsocktest suite for m68k (running it needs tier 2)
#   emulator     tier 2, boots FS-UAE, needs a ROM
#   cards        tier 2, boots EVERY network card this project supports,
#                one guest each, and proves each one carries bytes in
#                both directions.  Needs a bridge and a peer.
#   bridged      tier 2, the two pass/fail harnesses that need a real link:
#                NetShutdown against live services, and TCP: driven by
#                Commodore's own Type and Copy.  Needs a bridge, and a peer
#                for the second.
#   lossgate     tier 2, throughput on a link that loses packets, against a
#                recorded baseline.  The only rig that can price a change to
#                acknowledgement or retransmission behaviour.  Needs a bridge,
#                a peer, and tc on it.
#   smb          tier 2, mounts an SMB share on a real Workbench over our
#                stack, the way GitHub #3 and #4 describe.  Needs a licensed
#                Workbench, smb2fs, filesysbox and a peer to serve the share.
#   e2e          tier 2, installs the SHIPPED ARCHIVE on a real
#                Workbench 3.1, reboots, and drives it from another
#                machine: WebDAV, `lha x`, the browser terminal.
#                Needs a licensed Workbench, LhA, and a peer.
#
# `tools/ci.sh` with no arguments runs toolchain, host, host32, cross, web and
# conformance: everything that needs neither an emulator nor a licensed ROM.
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
    # Same class as the three below, and it went the same way: it changes the
    # layout of AmiRxSlot, and sana2_rx.c wrote a member the option compiles
    # out.  -DAMINETXDUO_RX_VERIFY=OFF did not build at all, and nothing said
    # so, because no arm turned it off.
    "norxverify:-DAMINETXDUO_RX_VERIFY=OFF"
    # The three TCP option flags that change the layout of NX_TCP_SOCKET and
    # are in no arm above.  SACK=OFF did not compile at all until it was built
    # here, and with all three on by default these arms are also the only place
    # each of them is compiled without the others.
    "nosack:-DAMINETXDUO_TCP_SACK=OFF"
    "nots:-DAMINETXDUO_TCP_TIMESTAMP=OFF"
    "nowscale:-DAMINETXDUO_TCP_WINDOW_SCALING=OFF"
    # The allocation census. Off in every other arm, so this is the only place
    # its side table, its redirect of ami_alloc and the reports it hangs off
    # bsd_lib_expunge are compiled at all. It is the instrument that finds a
    # leak nobody wrote a test for, and an instrument that stops building is
    # the one nobody notices.
    "census:-DAMINETXDUO_ALLOCCENSUS=ON"
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
HOST_TEST_TARGETS=(test_config test_usergroup test_mbuf test_bpf test_httppath test_httpif test_httplock test_argtemplates test_fetchurl test_crypto68k test_crypto68k_25519 test_net68k_checksum test_net68k_rxverify
                   test_tcp_retries test_bcast_loopback test_tcp_source_connect test_tcp_rtt
                   test_dns_retry test_dns_status
                   test_sockopt_numbers test_tls_expiry test_tls_resume test_sana2_copy test_sana2_tx test_sana2_rx test_sana2_driver test_ipv6_ra test_ipv6_ptb
                   test_httpframe test_httpws test_tls_x509 test_ipv6_frag test_iperfwire
                   fuzz_config fuzz_bpf fuzz_dns fuzz_usergroup
                   fuzz_dhcp fuzz_tls_record fuzz_tls_x509 fuzz_httpframe)

# test_inet exists only where tests/bsdsocket/CMakeLists.txt defines it, which
# is x86_64: ThreadX's linux tx_port.h types LONG as int there and as long
# everywhere else, and the shim cannot agree with both.  Asked for
# unconditionally, make answers "No rule to make target" on the arm64 runner
# and the whole host stage fails.
case "$(uname -m)" in
    x86_64|amd64) HOST_TEST_TARGETS+=(test_inet) ;;
esac

# How many ctest cases those targets register between them.  It is not the
# number of targets: test_crypto68k and friends register several each.
#
# THE NUMBER AND THE SUITE MOVE TOGETHER, and the check below is exact for
# that reason.  It used to be "fewer than this is a failure", which sounds
# safer and is not: every test added without touching this line left one more
# test's worth of slack, so by 2026-08-11 the bar was 47 against 49 real
# tests and two could have gone missing unnoticed.  A gate that drifts below
# what it guards is a gate that has already failed.
#
# Adding a test therefore turns CI red until this is raised.  That is the
# maintenance the gate is made of, and it is one line.
HOST_TESTS_EXPECTED=55
case "$(uname -m)" in
    x86_64|amd64) ;;
    *) HOST_TESTS_EXPECTED=$((HOST_TESTS_EXPECTED - 1)) ;;   # no test_inet
esac

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
# Work this run did NOT do.  A `note "... skipped"` in the middle of a hundred
# lines of build output is not a recorded status: it scrolls past and the
# summary still says "all green", so a runner missing gcc-multilib or cppcheck
# reported the same result as one that ran everything.  These are collected and
# listed under their own heading at the end.
SKIPPED=()

hr()   { printf '\n\033[1m======== %s\033[0m\n' "$*"; }
note() { printf '  %s\n' "$*"; }
fail() { FAILED+=("$1"); printf '\033[31m!! FAILED: %s\033[0m\n' "$1" >&2; }
skip() { SKIPPED+=("$1"); printf '\033[33m-- SKIPPED: %s\033[0m\n' "$1" >&2; }

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

    # The verdict the fourteen on-Amiga harnesses share, against ten fixtures
    # including a missing transcript and a check count under the floor.  It
    # needs no emulator, so it runs here: an assertion that stopped firing in
    # verdict_guest() would stop firing in all fourteen at once, and every one
    # of those needs a ROM to notice.
    if tools/test-verdict-selftest.sh > "$BUILD/verdict-selftest.log" 2>&1; then
        note "test-verdict selftest: $(sed -n 's/^verdict-selftest: //p' \
              "$BUILD/verdict-selftest.log")"
    else
        cat "$BUILD/verdict-selftest.log"
        fail "tools/test-verdict.sh selftest"
        return 1
    fi

    # The same argument one level down, for the harnesses that grade a
    # transcript rather than a check count.  run-addifup.sh is the gate for
    # "AddNetInterface never came back" and runs on one self-hosted runner, so
    # an assertion of its that stopped firing would be invisible here; these
    # selftests drive the graders against transcripts that stand for runs
    # nobody can produce on demand.
    # And that every harness has a home, and that the home is real.  A test
    # nothing invokes reads exactly like a test that passes.
    if tools/check-harnesses.sh > "$BUILD/check-harnesses.log" 2>&1; then
        note "harnesses: $(sed -n 's/^harnesses_wired=/wired /p' \
              "$BUILD/check-harnesses.log")$(sed -n 's/^harnesses_manual=/, manual /p' \
              "$BUILD/check-harnesses.log")$(sed -n 's/^harnesses_unwired=/, of those unwired /p' \
              "$BUILD/check-harnesses.log")"
    else
        cat "$BUILD/check-harnesses.log"
        fail "tests/HARNESSES does not match the tree (tools/check-harnesses.sh)"
        return 1
    fi

    # Every drawer in the archive is a configuration this script compiles.
    # The minimal drawer is not, and says so with its reason.
    if tools/check-shipping-config.sh > "$BUILD/shipping-config.log" 2>&1; then
        note "shipping config: $(grep -c '=matches_ci_arm_' \
              "$BUILD/shipping-config.log") of 4 drawers match their cross arm"
        grep '=KNOWN_DIVERGENCE' "$BUILD/shipping-config.log" |
            while read -r l; do note "$l"; done
    else
        cat "$BUILD/shipping-config.log"
        fail "a shipped drawer is built in a configuration no cross arm compiles"
        return 1
    fi

    local st log
    for st in tests/*/*-verdict-selftest.sh; do
        [ -x "$st" ] || continue
        log="$BUILD/$(basename "$st" .sh).log"
        if "$st" > "$log" 2>&1; then
            note "$(basename "$st"): $(sed -n 's/^.*selftest: //p' "$log")"
        else
            cat "$log"
            fail "$st"
            return 1
        fi
    done

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

    # Against the number of TESTS, not the number of BUILD TARGETS.  Several
    # targets register more than one ctest case, so comparing 45 registered
    # tests against 36 targets left nine of them free to disappear without the
    # gate noticing.  Same shape as the host32 stage below.
    local n want
    want="$HOST_TESTS_EXPECTED"
    n=$( (cd "$BUILD/host" && ctest -N 2>/dev/null | sed -n 's/^Total Tests: //p') )
    note "$n tests registered (expected $want)"
    if [ "${n:-0}" -lt "$want" ]; then
        fail "only $n tests registered, expected $want: tests have gone missing"
        return 1
    fi
    if [ "${n:-0}" -gt "$want" ]; then
        fail "$n tests registered, expected $want: tests were added without" \
             "raising HOST_TESTS_EXPECTED in tools/ci.sh, and every one of" \
             "them is slack the next removal can hide in"
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
        skip "host32: no -m32 on this host (needs gcc-multilib on Debian/Ubuntu),\
 the mDNS and TLS-crypto fuzz drivers did not run"
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
        skip "cross: no sfdc, the Developer drawer headers were NOT checked\
 against developer/sfd/aminetxduo_lib.sfd"
    fi

    # AMINETXDUO_CI_CROSS=default builds just one, what the emulator tier
    # needs, and what you want when bisecting.  A NAME THAT MATCHES NOTHING IS
    # AN ERROR: it used to skip all fourteen configurations and report green,
    # so `AMINETXDUO_CI_CROSS=deafult tools/ci.sh cross` compiled nothing at
    # all and said so nowhere.
    if [ -n "${AMINETXDUO_CI_CROSS:-}" ]; then
        local want unknown=""
        for want in $AMINETXDUO_CI_CROSS; do
            case " ${CROSS_CONFIGS[*]%%:*} " in
                *" $want "*) ;;
                *) unknown="$unknown $want" ;;
            esac
        done
        if [ -n "$unknown" ]; then
            hr "cross builds"
            echo "AMINETXDUO_CI_CROSS names no such configuration:$unknown" >&2
            echo "known:  ${CROSS_CONFIGS[*]%%:*}" >&2
            fail "AMINETXDUO_CI_CROSS=$AMINETXDUO_CI_CROSS matched nothing;\
 nothing was built"
            return 1
        fi
    fi

    for entry in "${CROSS_CONFIGS[@]}"; do
        name="${entry%%:*}"
        opts="${entry#*:}"

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
        skip "conformance: third_party/bsdsocktest is not checked out, the suite\
 was not built"
        return 0
    fi
    # -b, or conf_launcher is silently left out. build.sh looks for the crash
    # guard in build/cm, which this script never produces; without the flag it
    # printed one line on stderr, exited 0, and the next
    # tests/conformance/run-fsuae.sh stopped
    # with "missing conf_launcher" -- a build stage that reported success and
    # had not built the thing the run needs.
    tests/conformance/build.sh -b "$BUILD/default" \
        > "$BUILD/conformance.log" 2>&1 || {
        tail -30 "$BUILD/conformance.log"; fail "conformance build"; return 1; }
    if [ ! -f build/bsdsocktest/conf_launcher ]; then
        tail -5 "$BUILD/conformance.log"
        fail "conformance: conf_launcher was not built, so\
 tests/conformance/run-fsuae.sh cannot start the suite. It needs\
 $BUILD/default/src/common/libaminetxduo_common.a,\
 which the cross stage produces -- run that first."
        return 1
    fi
    note "$(grep -c '  CC ' "$BUILD/conformance.log" || true) translation units"
    note "build/bsdsocktest/bsdsocktest, build/bsdsocktest/conf_launcher"
}

# -------------------------------------------------------------- the web ----

stage_web() {
    hr "httpd's terminal page"

    #
    # src/tools/web/shell.html is COMMITTED and the m68k build only copies
    # it, so nothing about `cmake --build` needs node.  The price of that is
    # that the file can drift from the TypeScript it was generated from, and a
    # page a commit behind its sources is a page whose bug is already fixed in
    # a source nobody rebuilt.  This is the check that catches it.
    #
    # esbuild comes from npm and this may be a runner with no network, so a
    # missing node_modules is a SKIP with the command to fix it -- but a node
    # that is present and a page that does not match is a FAILURE.
    #
    if ! command -v node > /dev/null; then
        skip "web: node is not installed, shell.html was not checked against\
 its sources (node tools/web/build.mjs --check)"
        return 0
    fi

    if [ ! -d tools/web/node_modules/esbuild ]; then
        if ! (cd tools/web && npm ci --silent --no-audit --no-fund) \
                > "$BUILD/web-npm.log" 2>&1; then
            tail -5 "$BUILD/web-npm.log"
            skip "web: npm could not install the bundler, shell.html was not\
 checked (cd tools/web && npm ci)"
            return 0
        fi
    fi

    if node tools/web/build.mjs --check > "$BUILD/web.log" 2>&1; then
        note "$(cat "$BUILD/web.log")"
    else
        cat "$BUILD/web.log"
        fail "web (shell.html does not match src/tools/web/client)"
        return 1
    fi

    # The typings are the half esbuild does not do: it strips the types and
    # never reads them.
    if (cd tools/web && ./node_modules/.bin/tsc \
            -p ../../src/tools/web/tsconfig.json --noEmit) \
            > "$BUILD/web-tsc.log" 2>&1; then
        note "TypeScript clean"
    else
        cat "$BUILD/web-tsc.log"
        fail "web (tsc)"
        return 1
    fi

    # And every vendored drawer is meant to be upstream's, byte for byte.
    # Each carries its own PROVENANCE: a comment block and then a plain
    # sha256sum list, which is why the comments are stripped and the rest is
    # handed to sha256sum as it stands.
    for v in src/tools/web/vendor/*/; do
        [ -f "$v/PROVENANCE" ] || continue
        if (cd "$v" && grep -v '^#' PROVENANCE | grep . |
                sha256sum --check --status) 2> /dev/null; then
            note "vendored $(basename "$v") matches its recorded hashes"
        else
            fail "web ($v has been edited, see its PROVENANCE)"
            return 1
        fi
    done
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
        skip "analyze: off by default, AMINETXDUO_ANALYZE=1 runs it (the release\
 workflow does)"
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
        skip "analyze: cppcheck is not installed, that half of the stage did not\
 run"
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

# Which emulator runs the guests.  Amiberry, and only Amiberry: fs-uae needs an
# X server, and the 3.1.66 build has no SLIRP, so every guest booted, ran
# nothing and timed out with no DH0:.done, which reads exactly like a crash in
# the code under test.  All eight tier-2 tests failed that way and all eight
# pass under Amiberry.
EMU_RUNNER="${AMINETXDUO_EMU_RUNNER:-tools/amiberry-run.sh}"

stage_emulator() {
    hr "emulator tests (tier 2)"

    # Check the emulator this stage will actually use, not a different one.
    # This asked for fs-uae while running Amiberry, so a machine with only
    # Amiberry -- which is every machine that works -- was told tier 2 could
    # not run, and the eight tests it would have passed never started.
    if [ ! -x "$EMU_RUNNER" ]; then
        fail "$EMU_RUNNER is missing, tier 2 cannot run"
        return 1
    fi
    if ! command -v amiberry >/dev/null 2>&1 &&
       [ ! -x "${AMIBERRY:-}" ] && [ ! -x "$HOME/amiberry/build/amiberry" ]; then
        fail "amiberry is not installed, tier 2 cannot run"
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

    # Say this once, here, rather than letting four tests time out in turn.
    # An A600 booted with an A1200 ROM never reaches a Shell, and the only
    # evidence is an empty serial log, which is what a crash in the code under
    # test looks like too.
    if [ -z "${AMINETXDUO_KICKSTART_A600:-}" ]; then
        note "AMINETXDUO_KICKSTART_A600 is not set: the 68000 arm will fall back"
        note "  to AMINETXDUO_KICKSTART, and an A1200 ROM does not boot an A600."
        note "  Point it at the 40.63 A500/A600 image."
    elif [ ! -f "${AMINETXDUO_KICKSTART_A600}" ]; then
        fail "AMINETXDUO_KICKSTART_A600 is set to a file that does not exist"
        return 1
    fi

    local entry exe timeout dir cpuopt tag budget
    for dir in default m68000; do
        # These budgets multiply the per-test ceilings below.  They are ceilings,
        # not fixed waits, so a passing run finishes long before them and paying
        # for headroom is free, and the hosted GitHub runner is markedly slower
        # at emulation than a dedicated box, enough to time a test out on the old
        # 68020 x1 / 68000 x2 budgets.  Doubled so a slow runner has rope.
        if [ "$dir" = "m68000" ]; then
            # -m A600, not -c 68000.  Forcing cpu_type=68000 onto the default
            # A1200 makes a machine that does not exist: the quickstart puts up
            # an AGA board with a 68EC020 and the ROM to match, and the CPU line
            # then contradicts both.  It never reaches a Shell, so every test on
            # this arm timed out with an empty serial log and no DH0:.done,
            # which reads as eight broken tests rather than one bad config.
            #
            # The 68000 build itself was never the problem: it passes on the
            # A1200 arm.  A600 is a real 68000 machine and needs a 68000 ROM,
            # AMINETXDUO_KICKSTART_A600 (the 40.63 A500/A600 image); the default
            # AMINETXDUO_KICKSTART is an A1200 40.68 and will not boot one.
            cpuopt="-m A600";  tag="68000"; budget=4
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
            if AMINETXDUO_RUN_TAG="ci-$tag" "$EMU_RUNNER" $cpuopt \
                   -t "$timeout" "$BUILD/$dir/$exe" > "$log" 2>&1; then
                note "PASS  $(grep -E '[0-9]+ checks' "$log" | tail -1)"
            else
                tail -25 "$log"
                fail "emulator/$tag: $exe"
            fi
        done
    done
}

# ------------------------------------------------------------ release e2e ----
#
# The one stage that tests the SHIPPED ARCHIVE rather than a build tree, on a
# genuine Workbench 3.1, and then uses the machine the way its owner does:
# WebDAV from another machine, `lha x` on the Amiga, a Shell through the
# browser terminal.  A defect in what dist/make-dist.sh packs is invisible
# everywhere else in this script.
#
# NOT in the default set, and not in `emulator` either.  It needs a licensed
# Workbench 3.1 floppy set, Commodore's Installer, LhA from the asset store
# and a SECOND MACHINE that can reach the guest -- the host running amiberry
# cannot, see install/test/run-workbench.sh.  Naming it runs it.
#
# The ingredient list is not repeated here.  run-workbench.sh already owns it
# and exits 2 when something is missing, 3 when it could not talk to the
# machine from anywhere else, and those two are different things: the first is
# "this box cannot run this test", the second is "the test could not observe
# what it exists to observe".  Neither is a pass.
stage_e2e() {
    hr "release end-to-end (tier 2, needs a licensed Workbench and a peer)"

    local archive="${AMINETXDUO_E2E_ARCHIVE:-}"
    local rc=0

    if [ -z "$archive" ]; then
        local version
        version=$("$ROOT/tools/version.sh" --product)
        archive="$ROOT/build/dist/AmiNetXDuo-$version.lha"
    fi
    if [ ! -f "$archive" ]; then
        fail "no release archive at $archive; run dist/make-dist.sh first," \
             "or set AMINETXDUO_E2E_ARCHIVE"
        return 1
    fi
    note "archive: $archive"

    "$ROOT/install/test/run-workbench.sh" -l AVERAGE -H -a "$archive" || rc=$?

    case "$rc" in
        0) note "PASS  the shipped archive installs, boots, serves and unpacks" ;;
        2) fail "release e2e: an ingredient is missing on this machine" ;;
        3) fail "release e2e: no second machine could reach the Amiga" ;;
        *) fail "release e2e: exit $rc" ;;
    esac
    return "$rc"
}

# ----------------------------------------------------------------- cards ----
#
# EVERY network card, one boot each, and each one asserted the same way: the
# interface comes online and bytes move in both directions, counted by a
# machine that is not this one.
#
# NOT in the default set, and not in `emulator` either, for the same two
# reasons `e2e` is not: it needs a BRIDGE, which means a host NIC and
# CAP_NET_RAW on the emulator binary, and it needs a THIRD MACHINE, because a
# frame the emulator's host sends to its own bridged guest never comes back to
# that NIC's pcap.  Naming it runs it.
#
# It is also the longest thing in this file -- nine guests, boot to verdict --
# so it belongs to the nightly and not to a push.
#
# Until this existed, ONE card was ever booted by CI, the a2065 in
# install/test/run-workbench.sh, and the other eight were hand-run in a
# throwaway tree.  x-surf-100 reached a user broken.
stage_cards() {
    hr "network cards (tier 2, needs a bridge and a peer)"

    local rc=0

    if [ -z "${AMINETXDUO_CARDSWEEP_PEER:-}" ]; then
        skip "card sweep: AMINETXDUO_CARDSWEEP_PEER is not set, so there is no" \
             "machine off this box to count the bytes.  Every card is unproven" \
             "on this runner."
        return 0
    fi

    "$ROOT/tests/tools/run-cardsweep.sh" -b "$BUILD/default" || rc=$?

    # 1 is the CARD claim -- online, and bytes both ways agreeing with a
    # machine off this box -- and it is the only one that turns this red.
    # 3 is every card carrying traffic with something else wrong, which is
    # loud here and in the table and is not a build break: the arm around the
    # claim is flaky in a way the claim is not, and a nightly that goes red on
    # a coin toss stops being read.  run-cardsweep.sh says why at length.
    case "$rc" in
        0) note "PASS  every card came online and carried bytes both ways" ;;
        1) fail "card sweep: a card did not carry traffic -- the table above" \
                "names it and says which direction failed" ;;
        2) fail "card sweep: the rig refused it before any card was measured" ;;
        3) skip "card sweep: every card carried bytes both ways, and an arm" \
                "failed or a card was not measured -- the table above says" \
                "which and why" ;;
        *) fail "card sweep: exit $rc" ;;
    esac
    return "$rc"
}

# --------------------------------------------------------------- bridged ----
#
# The two harnesses that are ordinary pass/fail regression tests and happen to
# need a real link.  Both existed, both were green, and neither was invoked by
# anything: run-netshutdown.sh is the regression test for a defect a user
# reported (programs holding bsdsocket.library were never told the network had
# gone), and run-tcphandler.sh is the only thing that runs Commodore's own
# Type and Copy over a socket.
#
# NOT in `emulator`: that stage boots a bare drive with no network device at
# all.  These need the a2065 and a NIC to bridge onto, which is the same
# requirement `cards` has minus the peer -- run-netshutdown.sh starts its own
# services and shuts them down, so nothing has to call in.  run-tcphandler.sh
# does need a peer, and says so per harness rather than taking the stage down.
stage_bridged() {
    hr "bridged harnesses (tier 2, needs a real link)"

    local rc=0 bad=0

    printf '\n-- NetShutdown, and the programs using the network\n'
    "$ROOT/tests/tools/run-netshutdown.sh" -b "$BUILD/default" || rc=$?
    case "$rc" in
        0) note "PASS  both arms: the cooperative one let go, the deaf one" \
                "was reported and left alone" ;;
        2) fail "netshutdown: an ingredient is missing on this machine" ; bad=1 ;;
        *) fail "netshutdown: the transcript above is the whole run" ; bad=1 ;;
    esac

    # The peer that carries tc and tcpdump with capabilities, which is the
    # same one tests/perf/run-lossgate.sh needs and not AMINETXDUO_PEER: this
    # drill loses the guest's first ARP on purpose and cannot do it from here.
    printf '\n-- one lost ARP must not fail a TCP arm\n'
    if [ -z "${AMINETXDUO_FITZ_PEER:-}" ]; then
        skip "arpretry: AMINETXDUO_FITZ_PEER is not set, so there is no peer" \
             "that can drop the guest's first ARP.  A lost ARP is untested" \
             "on this runner."
    else
        rc=0
        "$ROOT/tests/tools/run-arpretry.sh" -b "$BUILD/default" \
            -B "${AMINETXDUO_AMIBERRY_BACKEND:-ens18}" \
            -P "$AMINETXDUO_FITZ_PEER" || rc=$?
        case "$rc" in
            0) note "PASS  the first ARP was lost and the connect still" \
                    "completed inside the window the application waits" ;;
            2) fail "arpretry: an ingredient is missing, or the drill" \
                    "measured nothing -- read arp_reqs and netem_dropped" ; bad=1 ;;
            *) fail "arpretry: the transcript above is the whole run" ; bad=1 ;;
        esac
    fi

    printf '\n-- TCP: is an AmigaDOS device, and stock commands use it\n'
    if [ -z "${AMINETXDUO_PEER:-}" ]; then
        skip "tcphandler: AMINETXDUO_PEER is not set, so there is no third" \
             "machine to answer the guest.  TCP: is unproven on this runner."
    elif [ -z "${AMINETXDUO_AMIGA_C:-}" ]; then
        skip "tcphandler: AMINETXDUO_AMIGA_C is not set.  The whole point is" \
             "that STOCK commands work, so it will not substitute ours for" \
             "Commodore's Type and Copy."
    else
        rc=0
        "$ROOT/tests/tools/run-tcphandler.sh" -b "$BUILD/default" \
            -B "${AMINETXDUO_AMIBERRY_BACKEND:-ens18}" \
            -P "$AMINETXDUO_PEER" || rc=$?
        case "$rc" in
            0) note "PASS  Type, Copy and Shell redirection over a socket" ;;
            2) fail "tcphandler: an ingredient is missing on this machine" ; bad=1 ;;
            *) fail "tcphandler: the transcript above is the whole run" ; bad=1 ;;
        esac
    fi

    # The stall arm is here rather than in the emulator stage because the
    # question is a retransmission ladder against a real peer.  It costs its
    # own two minutes: the socket that asked for nothing has to be watched all
    # the way to 127 s, or "the default is unchanged" is not being tested.
    printf '\n-- a peer that stops answering, and who can tell\n'
    if [ -z "${AMINETXDUO_PEER:-}" ]; then
        skip "tcpstall: AMINETXDUO_PEER is not set, so there is no peer to" \
             "stall against.  A stalled connection is unproven on this runner."
    else
        rc=0
        "$ROOT/tests/tcpstall/run-tcpstall.sh" -b "$BUILD/default" \
            -B "${AMINETXDUO_AMIBERRY_BACKEND:-ens18}" \
            -P "$AMINETXDUO_PEER" || rc=$?
        case "$rc" in
            0) note "PASS  the stall was readable while it ran, the deadline" \
                    "was served, and the default socket still took 127 s" ;;
            2) fail "tcpstall: an ingredient is missing on this machine" ; bad=1 ;;
            *) fail "tcpstall: the transcript above is the whole run" ; bad=1 ;;
        esac
    fi

    return "$bad"
}

# -------------------------------------------------------------- lossgate ----
#
# Our lab rig measures zero retransmissions, so a change to acknowledgement or
# retransmission behaviour costs nothing on it and 0.16.6 shipped two of them.
# This is the only arm where such a change has a price, and it is a GATE: the
# medians are compared against tests/perf/lossgate-baseline.txt, read and write
# separately, because that regression moved them in opposite directions.
#
# The peer's root qdisc is changed for the duration, so it must be idle; the
# harness refuses to start otherwise rather than producing a number nobody can
# trust.
stage_lossgate() {
    hr "the lossy-link gate (tier 2, needs a peer with tc)"

    if [ -z "${AMINETXDUO_FITZ_PEER:-}" ] ||
       [ -z "${AMINETXDUO_FITZ_PEER_ADDR:-}" ]; then
        skip "lossgate: AMINETXDUO_FITZ_PEER and AMINETXDUO_FITZ_PEER_ADDR" \
             "are not both set, so there is no machine to induce loss from." \
             "A change to acking or retransmission is unpriced on this runner."
        return 0
    fi

    local rc=0
    AMINETXDUO_BUILD="$BUILD/default" \
        "$ROOT/tests/perf/run-lossgate.sh" \
            -H "$AMINETXDUO_FITZ_PEER" -A "$AMINETXDUO_FITZ_PEER_ADDR" \
            -b "$BUILD/default" || rc=$?

    case "$rc" in
        0) note "PASS  read, write and retransmits are all within tolerance" \
                "of tests/perf/lossgate-baseline.txt" ;;
        2) fail "lossgate: the rig refused it -- no baseline, or the peer is" \
                "busy.  The line above says which." ;;
        *) fail "lossgate: a metric moved past its tolerance; the table above" \
                "names it.  READ is the field metric." ;;
    esac
    return "$rc"
}

# ------------------------------------------------------------------- smb ----
#
# GitHub #3 and #4 were both unreproducible for a day because SMB had never
# been run against this stack at all.  A socket test cannot reach either: `nc`
# already connects to 445 from a booted A1200, so whatever fails happens above
# the handshake, inside smb2-handler, and only a machine that can Mount shows
# it.  That means a real Workbench, Commodore's Mount, and a share.
#
# The share is the harness's own (-P), not one that happens to be up on the
# LAN: a server nobody records is a test that fails for reasons that are not
# the stack's.
stage_smb() {
    hr "SMB on a real Workbench (tier 2, needs a Workbench and a peer)"

    if [ -z "${AMINETXDUO_PEER:-}" ]; then
        skip "smb: AMINETXDUO_PEER is not set, so there is nowhere to serve a" \
             "share from that the guest can reach.  SMB is unproven on this" \
             "runner, which is the state issues #3 and #4 were reported in."
        return 0
    fi

    local arm rc bad=0 flag
    # Two arms because ACTIVATE decides WHICH PROCESS connects: without it
    # Mount only builds the device node and the first access starts the
    # handler, with it Mount does.  #4 is a freeze with no error printed, and
    # which process froze is the first thing to know.
    for arm in lazy activate; do
        printf '\n-- mount, %s\n' "$arm"
        rc=0
        flag=""
        [ "$arm" = activate ] && flag="-A"
        AMINETXDUO_BUILD="$BUILD/default" \
            "$ROOT/install/test/run-smbmount.sh" -P "$AMINETXDUO_PEER" \
                -T "ci-smb-$arm" ${flag:+"$flag"} || rc=$?
        case "$rc" in
            0) note "PASS  the share mounted and listed ($arm)" ;;
            2) fail "smb/$arm: an ingredient is missing on this machine" ; bad=1 ;;
            *) fail "smb/$arm: the verdict= line above says whether it never" \
                    "connected, or connected and stopped" ; bad=1 ;;
        esac
    done
    return "$bad"
}

# ------------------------------------------------------------------ main ----

mkdir -p "$BUILD"

#
# analyze is NOT in the default set. It is 2.5 minutes of the roughly four a
# default run takes, and its findings do not move between commits the way a
# build break does, the baseline has sat at 13 for weeks. Naming it runs it:
#
#     tools/ci.sh analyze                  just it
#     tools/ci.sh host host32 cross web analyze conformance    the release set
#
# The release workflow names it, so nothing ships unanalysed. A default run
# says out loud that it skipped, because a stage that goes quiet reads as
# coverage it is not providing.
#
WANT=("$@")
[ ${#WANT[@]} -gt 0 ] || WANT=(host host32 cross web conformance)

stage_submodules

# Anything but a pure host run needs the cross compiler.
for s in "${WANT[@]}"; do
    case "$s" in
        cross|analyze|conformance|emulator|e2e|cards|bridged|lossgate|smb)
            stage_toolchain; break ;;
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
        web)         stage_web || true ;;
        analyze)     stage_analyze || true ;;
        conformance) stage_conformance || true ;;
        emulator)    stage_emulator || true ;;
        cards)       stage_cards || true ;;
        bridged)     stage_bridged || true ;;
        lossgate)    stage_lossgate || true ;;
        smb)         stage_smb || true ;;
        e2e)         stage_e2e || true ;;
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
if [ ${#SKIPPED[@]} -ne 0 ]; then
    printf '\033[33m%d skipped:\033[0m\n' "${#SKIPPED[@]}"
    printf '  - %s\n' "${SKIPPED[@]}"
    # And into the run summary, where a person looks.  A stage that skipped
    # half of itself -- tcphandler with no peer, the card sweep with nowhere to
    # count from -- exits 0 and scrolls past in a step that reads as a tick.
    # tools/ci-arm.sh records the arms; this records the halves inside them.
    if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
        {
            echo
            echo "<details><summary><strong>${#SKIPPED[@]} thing(s) tools/ci.sh ${STAGES_RUN[*]} did NOT test</strong></summary>"
            echo
            printf -- '- %s\n' "${SKIPPED[@]}"
            echo
            echo "</details>"
        } >> "$GITHUB_STEP_SUMMARY"
    fi
fi

if [ ${#FAILED[@]} -eq 0 ]; then
    if [ ${#SKIPPED[@]} -eq 0 ]; then
        printf '\033[32mall green\033[0m\n'
    else
        printf '\033[32mgreen\033[0m, with %d thing(s) NOT tested (above)\n' \
               "${#SKIPPED[@]}"
    fi
    exit 0
fi
printf '\033[31m%d failure(s):\033[0m\n' "${#FAILED[@]}"
printf '  - %s\n' "${FAILED[@]}"
exit 1
