#!/usr/bin/env bash
#
# Everything CI does, in one script, so it can be run before pushing.
#
#   tools/ci.sh                      # tier 1: host tests + every cross config
#   tools/ci.sh host                 # just the host tests
#   tools/ci.sh cross                # just the cross builds
#   tools/ci.sh emulator             # tier 2: FS-UAE, needs a boot ROM
#   tools/ci.sh cards                # tier 2: every network card, one boot each
#   tools/ci.sh cards6               # tier 2: every card, IPv6 past the router
#   tools/ci.sh capture              # tier 2: NetCapture, every card
#   tools/ci.sh wirequiet            # tier 2: what an idle machine emits
#   tools/ci.sh reachability         # tier 2: still answering during a handshake
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
#   sanitize     the whole host tier again, 64-bit and 32-bit, built under
#                ASan+UBSan with leaks fatal.  Not in the default set: name
#                it, or set AMINETXDUO_SANITIZE=1.  CI does both.
#   cross        every build configuration, warnings fatal
#   web          httpd's two pages, the terminal's and the console's, still
#                match the TypeScript they are generated from, and the
#                vendored xterm.js is untouched
#   analyze      GCC -fanalyzer over our own sources vs a triaged baseline
#   conformance  build the bsdsocktest suite for m68k (running it needs tier 2)
#   emulator     tier 2, boots FS-UAE, needs a ROM
#   cards        tier 2, boots EVERY network card this project supports,
#                one guest each, and proves each one carries bytes in
#                both directions.  Needs a bridge and a peer.
#   cards6       tier 2, boots EVERY network card again and asks a different
#                question: does IPv6 reach anything PAST THE ROUTER.  Needs a
#                bridge and a working IPv6 delegation on it; no peer.
#   capture      tier 2, boots EVERY card again and points NetCapture at a
#                real segment: the frames are ping's, the filter is checked
#                against an unfiltered capture of the same seconds, and
#                tcpdump on another machine reads the files.  Needs a bridge;
#                a peer makes the last claim stronger.
#   wirequiet    tier 2, boots EVERY card and asks what the machine puts on
#                the wire when nobody asked it to: a settle, then a window of
#                idle counted off this host's own NIC with tcpdump.  Nothing
#                else in this tree asserts on what the guest EMITS, which is
#                how a DHCPv6 client rebinding eight times a second passed
#                every stage for as long as it did.  Needs a bridge and an
#                unprivileged tcpdump; no peer.
#   reachability tier 2, does the machine still answer while it is doing a
#                TLS handshake.  A peer probes it every two seconds through a
#                real https fetch, and the gate is the longest stretch with no
#                answer.  Needs a bridge and a peer that is not this host.
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
#   e2ecards     tier 2, the same install and power cycle ON EVERY CARD in
#                tests/tools/cards.sh rather than on the A2065 alone.  Needs
#                a licensed Workbench and the vendor drivers in the asset
#                store; no peer, so it is not `e2e` with a loop round it.
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
# EXIT CODES
#
#   0   the stages ran and nothing failed
#   1   something failed; the list at the end names it
#   2   a stage name that does not exist
#   77  IT TESTED NOTHING.  Every stage asked for skipped outright -- no peer,
#       no bridge, no -m32 -- so there is no result here at all, in either
#       direction.  This used to be 0, and `tools/ci.sh cards` on a runner with
#       AMINETXDUO_CARDSWEEP_PEER unset reported nine network cards proved when
#       it had booted none of them.  tools/ci-arm.sh renders 77 as SKIPPED.
#       A run where ANY stage produced a verdict is 0 or 1 as before.
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
# THE CPU ARMS ARE GONE, and their absence is the point: the archive used to
# carry a library per CPU and each -m flag had to compile, so three full cross
# builds ran here to prove three binaries that one binary now replaces.  What
# the tree may contain no longer depends on the flag -- every part of the data
# path and the crypto that needs a 68020 instruction is assembled for the part
# that needs it and reached through a vector chosen from AttnFlags at init
# (src/net68k/n68k_cpu.c, src/crypto68k/c68k_cpu.c), so the `default` arm is
# the shipping arm and compiles all of it.
#
# `default` and `minimal` are the two libraries the archive ships, in the
# options the release workflow gives them, so a break in either is a break in
# something a user downloads.  Nothing may ship in a shape that is not built
# here.
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
    # ONE PER-CPU ARM, and only one.  There used to be three -- m68000, m68040
    # and m68060 -- because the archive shipped a library per CPU and each had
    # to compile; that is over, one build serves every 68k and chooses its
    # inner loops from AttnFlags at init, so `default` IS the shipping arm.
    #
    # 68060 came back because it is not a tuning choice.  -DAMINETXDUO_CPU is a
    # cache variable with a STRINGS list, so it is a configuration a user can
    # select, and of the five values it is the only one whose flag names a
    # different TARGET: 68000 and `any` are -m68000, which `default` compiles,
    # 68020 and 68040 are -m68020 whose extra instructions the default arm
    # already assembles in src/net68k and src/crypto68k, and -mcpu=68060 is a
    # part with instructions REMOVED.  Nothing built it, so nothing noticed
    # that src/common/ami_udivdi3.c left the assembler in 68000 mode for the
    # rest of the file and the whole tree stopped building for it.  One arm,
    # ~18 s of the cross stage on a 24-core host, one in fourteen.
    "cpu68060:-DAMINETXDUO_CPU=68060"
    # The second drawer in the archive, and the only arm here that turns more
    # than one thing off at once.  Every option above is a separate arm because
    # each has its own compile-time surface; this one exists because the
    # combination is what a user downloads, and the arms above do not cover it
    # BPF=OFF appears nowhere else at all, and the interactions between five
    # of them appear nowhere else at all.  It must stay byte-for-byte the
    # options .github/workflows/release.yml gives build/release-minimal.
    "minimal:-DAMINETXDUO_IPV6=OFF -DAMINETXDUO_MDNS=OFF -DAMINETXDUO_BPF=OFF -DAMINETXDUO_TLS=OFF -DAMINETXDUO_MULTICAST=OFF -DAMINETXDUO_AREXX=OFF -DAMINETXDUO_TCPDEVICE=OFF"
)

# WHAT THE HOST STAGES BUILD IS NOT WRITTEN DOWN HERE ANY MORE.
#
# It used to be a hand-kept array, and three times a test was registered whose
# binary the array did not name.  Each time it passed in a working tree, where
# the binary was already lying about from an earlier build, and each time CI
# reported "Not Run" on a clean configure: the netdev host tests, then rfbgen
# and rfbbench under rfb_roundtrip, then test_netdev_el3.  A list with no
# mechanical relationship to what it mirrors drifts, and this one did.
#
# cmake/HostTests.cmake wraps add_test() and computes the targets from the
# registrations, writing them to host-test-targets.txt in the build directory.
# host_test_targets() below reads that file.  Adding a test now needs no edit
# here at all, and a registration naming a target that does not exist is a
# FATAL_ERROR at configure rather than a "Not Run" after the build.
#
# Read per build directory, not once: the sanitize configure can select a
# different set from the plain one, and used to be given the plain one's.
host_test_targets() { # builddir
    local f="$1/host-test-targets.txt"

    if [ ! -s "$f" ]; then
        fail "no $f: the configure did not write the host test target list," \
             "so cmake/HostTests.cmake did not run"
        return 1
    fi
    tr '\n' ' ' < "$f"
}

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
HOST_TESTS_EXPECTED=94
case "$(uname -m)" in
    x86_64|amd64) ;;
    # test_inet, test_route, test_expunge, test_select and test_sockopt, all
    # x86_64-only for the reason in tests/bsdsocket/CMakeLists.txt: elsewhere
    # the host's LONG is eight bytes and no structure in them has the
    # target's shape.
    *) HOST_TESTS_EXPECTED=$((HOST_TESTS_EXPECTED - 5)) ;;
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
    # The dual stack over the same simulated wire ram_driver_test uses: two
    # NX_IP instances, both ends inside the emulator, so it needs no card, no
    # driver and nothing outside the guest.  85 checks in 18 s on the 68020
    # arm, measured 2026-08-20.
    #
    # It had been built by the `default` cross configuration since milestone 8
    # and executed by nothing: its CMakeLists names an amiberry-run.sh command
    # line to type by hand, and a command line in a comment is not a gate.
    # Only built with IPv6 on, which `default` is.
    "tests/ipv6/ipv6_test:120"
)

FAILED=()
STAGES_RUN=()
# Work this run did NOT do.  A `note "... skipped"` in the middle of a hundred
# lines of build output is not a recorded status: it scrolls past and the
# summary still says "all green", so a runner missing gcc-multilib or cppcheck
# reported the same result as one that ran everything.  These are collected and
# listed under their own heading at the end.
SKIPPED=()

# What a stage returns when it tested NOTHING: it reached its own gate, the
# gate said no, and no assertion in it ran.  Distinct from 0, which every such
# stage used to return, and from a failure.  See EXIT CODES at the top.
NOTHING=77

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

    # Which toolchain a build picks, against a fake cache.  The emulator tier
    # ran under GCC 15.2 for three releases: <cache>/current addressed a tree
    # that was not the pin, and both resolvers took it on "it runs" alone.
    # The check needs no toolchain and no network, and the failure it covers
    # was invisible everywhere except one generated-header diff.
    if tools/toolchain-resolve-selftest.sh > "$BUILD/tcresolve.log" 2>&1; then
        note "toolchain resolve selftest: $(sed -n \
              's/^toolchain_resolve_selftest=//p' "$BUILD/tcresolve.log")"
    else
        cat "$BUILD/tcresolve.log"
        fail "the toolchain resolvers accept a cache that is not the pin"
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

    # No harness may build a guest binary for a newer CPU than the machine it
    # then points that binary at.  It costs a day every time: the program stops
    # in its own C constructor, no serial, no stdout.txt, and it reads as "the
    # stack does not work on a 68000".
    #
    # THE LINT EXISTED AND NOTHING RAN IT, which is the state its own three
    # case studies were found in.  It was also red on `main`, on three scripts
    # that hardcode -m68000 -- the SAFE direction, which runs on every machine
    # in the matrix and is never this bug; it flags -m68020 and newer now.
    if tools/lint-guest-arch.sh > "$BUILD/guest-arch.log" 2>&1; then
        note "guest arch: $(sed -n 's/^guest-arch: //p' "$BUILD/guest-arch.log")"
    else
        cat "$BUILD/guest-arch.log"
        fail "a harness builds a guest binary for a newer CPU than the machine\
 it runs it on (tools/lint-guest-arch.sh)"
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
    local targets
    targets=$(host_test_targets "$BUILD/host") || return 1
    # shellcheck disable=SC2086
    cmake --build "$BUILD/host" --parallel "$JOBS" \
        --target $targets || { fail "host build"; return 1; }

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
        return "$NOTHING"
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

# -------------------------------------------------------------- sanitize ----

# The host tier again, both widths, built under -fsanitize=address,undefined
# with recovery off and leaks fatal.  The target cannot run a sanitizer, so
# this is the only place a stray write in the portable stack sources faults
# instead of corrupting another task.  Gated like analyze: out of the default
# stage list, run by naming it or by AMINETXDUO_SANITIZE=1, and CI does both,
# so nothing lands unsanitized.

stage_sanitize() {
    # Not in the default stage list, and that is all the gating there is.
    # Naming the stage runs it; so does AMINETXDUO_SANITIZE=1, which the main
    # loop turns into a named stage.  A second refusal in here made both forms
    # of the ask do nothing, which is what stage_analyze's comment describes.
    hr "sanitizers (host, 64-bit)"

    # Leak reports fail the run wherever the runtime has a detector.  Linux
    # ASan turns LSan on by itself; macOS carries the detector but off, and on
    # some Apple toolchains it is absent entirely, so probe rather than assume:
    # asking for a detector that is not there aborts every test at startup.
    local leakopt="" probe="$BUILD/sanprobe"
    if echo 'int main(void){return 0;}' > "$probe.c" &&
       "${CC:-cc}" -fsanitize=address "$probe.c" -o "$probe" 2>/dev/null &&
       ASAN_OPTIONS=detect_leaks=1 "$probe" >/dev/null 2>&1; then
        leakopt="detect_leaks=1"
    else
        case "$(uname -s)" in
            Linux) leakopt="detect_leaks=1" ;;   # on by default anyway
            *) skip "sanitize: no leak detector in this host's ASan runtime,\
 leaks were NOT checked here (the Linux runner checks them)" ;;
        esac
    fi
    export ASAN_OPTIONS="${leakopt:+$leakopt:}${ASAN_OPTIONS:-}"
    export UBSAN_OPTIONS="print_stacktrace=1:${UBSAN_OPTIONS:-}"

    cmake -S . -B "$BUILD/san" \
        -DCMAKE_BUILD_TYPE=Release \
        -DAMINETXDUO_SANITIZE=ON \
        -DCMAKE_PROJECT_INCLUDE="$ROOT/cmake/ci-warnings.cmake" \
        > "$BUILD/san-configure.log" 2>&1 || {
            tail -30 "$BUILD/san-configure.log"; fail "sanitize configure"; return 1; }
    note "$(grep -o 'sanitize: .*' "$BUILD/san-configure.log" | head -1)"

    local targets
    targets=$(host_test_targets "$BUILD/san") || return 1
    # shellcheck disable=SC2086
    cmake --build "$BUILD/san" --parallel "$JOBS" \
        --target $targets || { fail "sanitize build"; return 1; }

    ( cd "$BUILD/san" && ctest --output-on-failure ) || { fail "sanitize ctest"; return 1; }

    # Exact, both directions, for stage_host's reason: a gate that drifts
    # below what it guards has already failed.
    local n want
    want="$HOST_TESTS_EXPECTED"
    n=$( (cd "$BUILD/san" && ctest -N 2>/dev/null | sed -n 's/^Total Tests: //p') )
    note "$n sanitized tests ran (expected $want)"
    if [ "${n:-0}" -ne "$want" ]; then
        fail "sanitize registered $n tests, expected $want"
        return 1
    fi

    hr "sanitizers (host, 32-bit)"

    # m68k is ILP32, and the host32 drivers exist for the type-width
    # assumptions a 64-bit build cannot represent, so they get sanitized too.
    if ! (echo 'int main(void){return 0;}' > "$BUILD/m32probe.c" &&
          "${CC:-cc}" -m32 "$BUILD/m32probe.c" -o "$BUILD/m32probe") 2>/dev/null; then
        skip "sanitize32: no -m32 on this host (needs gcc-multilib on\
 Debian/Ubuntu), the mDNS and TLS-crypto drivers were not sanitized here"
        return 0
    fi

    cmake -S . -B "$BUILD/san32" \
        -DCMAKE_BUILD_TYPE=Release \
        -DAMINETXDUO_SANITIZE=ON \
        -DCMAKE_C_FLAGS=-m32 -DCMAKE_CXX_FLAGS=-m32 -DCMAKE_EXE_LINKER_FLAGS=-m32 \
        > "$BUILD/san32-configure.log" 2>&1 || {
            tail -30 "$BUILD/san32-configure.log"; fail "sanitize32 configure"; return 1; }

    # The configure degrades to UBSan alone where -m32 has no ASan runtime.
    # That is a recorded gap, not a quiet one.
    local mode
    mode=$(grep -o 'sanitize: .*' "$BUILD/san32-configure.log" | head -1)
    note "$mode"
    case "$mode" in
        *undefined\ only*) skip "sanitize32: no 32-bit ASan runtime on this\
 host, the 32-bit tier ran under UBSan alone" ;;
    esac

    cmake --build "$BUILD/san32" --parallel "$JOBS" \
        --target "${HOST32_TEST_TARGETS[@]}" \
        || { fail "sanitize32 build"; return 1; }

    ( cd "$BUILD/san32" && ctest --output-on-failure -R 'mdns|tls_crypto' ) \
        || { fail "sanitize32 ctest"; return 1; }

    local n32 want32
    want32=$(( ${#HOST32_TEST_TARGETS[@]} * 2 ))
    n32=$( (cd "$BUILD/san32" && ctest -N -R 'mdns|tls_crypto' 2>/dev/null | sed -n 's/^Total Tests: //p') )
    note "$n32 sanitized 32-bit tests ran (expected $want32)"
    if [ "${n32:-0}" -ne "$want32" ]; then
        fail "sanitize32 registered $n32 tests, expected $want32"
        return 1
    fi
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

    # Same argument one file over, and it had already gone wrong.  The LVO
    # table is generated from the NDK pragmas, and the four RFC 3493 if_*
    # vectors at -0x372..-0x384 had been added to the OUTPUT by hand, so the
    # generator no longer reproduced its own files: the next regeneration
    # would have silently deleted four shipped LVOs.  Nothing ran --check,
    # which is why nothing said so.  This runs it.
    if python3 tools/gen_vectors.py \
           --ndk "$AMIGA_TOOLCHAIN_ROOT/m68k-amigaos/ndk-include" \
           --check > "$BUILD/gen-vectors.log" 2>&1; then
        note "LVO table reproduces: $(tail -1 "$BUILD/gen-vectors.log")"
    else
        cat "$BUILD/gen-vectors.log"
        fail "src/bsdsocket/bsdsocket_vectors.[ch] do not match\
 tools/gen_vectors.py"
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

            # And that no runtime helper became a call to itself.  It links,
            # exports the right symbol, and eats the stack; see the script.
            if tools/check-rt-recursion.sh "$BUILD/$name" \
                    > "$BUILD/$name-rt-recursion.log" 2>&1; then
                note "$(sed -n 's/^rt_recursion=/runtime helpers: /p' \
                      "$BUILD/$name-rt-recursion.log" | head -1)"
            elif grep -q 'rt_recursion=skipped' "$BUILD/$name-rt-recursion.log"; then
                skip "cross: $(cat "$BUILD/$name-rt-recursion.log")"
            else
                cat "$BUILD/$name-rt-recursion.log"
                fail "a compiler runtime helper calls itself ($name)"
            fi
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
        return "$NOTHING"
    fi
    # -b, or conf_launcher is silently left out. build.sh looks for the crash
    # guard in build/cm, which this script never produces; without the flag it
    # printed one line on stderr, exited 0, and the next
    # tests/conformance/run-conformance.sh stopped with "missing
    # conf_launcher" -- a build stage that reported success and had not built
    # the thing the run needs.
    tests/conformance/build.sh -b "$BUILD/default" \
        > "$BUILD/conformance.log" 2>&1 || {
        tail -30 "$BUILD/conformance.log"; fail "conformance build"; return 1; }
    if [ ! -f build/bsdsocktest/conf_launcher ]; then
        tail -5 "$BUILD/conformance.log"
        fail "conformance: conf_launcher was not built, so\
 tests/conformance/run-conformance.sh cannot start the suite. It needs\
 $BUILD/default/src/common/libaminetxduo_common.a,\
 which the cross stage produces -- run that first."
        return 1
    fi
    note "$(grep -c '  CC ' "$BUILD/conformance.log" || true) translation units"
    note "build/bsdsocktest/bsdsocktest, build/bsdsocktest/conf_launcher"
}

# -------------------------------------------------------------- the web ----

stage_web() {
    hr "httpd's pages"

    #
    # src/tools/web/shell.html and src/tools/web/console.html are COMMITTED
    # and the m68k build only copies them, so nothing about `cmake --build`
    # needs node.  The price of that is that a file can drift from the
    # TypeScript it was generated from, and a page a commit behind its sources
    # is a page whose bug is already fixed in a source nobody rebuilt.  This is
    # the check that catches it.
    #
    # BOTH pages, each by its own builder: they share no bundle -- the Shell's
    # carries a vendored terminal and two webfonts, the console's carries a
    # planar decoder -- and a stage that checked one of the two would let the
    # other rot exactly as far.
    #
    # esbuild comes from npm and this may be a runner with no network, so a
    # missing node_modules is a SKIP with the command to fix it -- but a node
    # that is present and a page that does not match is a FAILURE.
    #
    if ! command -v node > /dev/null; then
        skip "web: node is not installed, shell.html and console.html were not\
 checked against their sources (node tools/web/build.mjs --check)"
        return "$NOTHING"
    fi

    if [ ! -d tools/web/node_modules/esbuild ]; then
        if ! (cd tools/web && npm ci --silent --no-audit --no-fund) \
                > "$BUILD/web-npm.log" 2>&1; then
            tail -5 "$BUILD/web-npm.log"
            skip "web: npm could not install the bundler, the pages were not\
 checked (cd tools/web && npm ci)"
            return "$NOTHING"
        fi
    fi

    if node tools/web/build.mjs --check > "$BUILD/web.log" 2>&1; then
        note "$(cat "$BUILD/web.log")"
    else
        cat "$BUILD/web.log"
        fail "web (shell.html does not match src/tools/web/client)"
        return 1
    fi

    if node tools/web/build-console.mjs --check \
            > "$BUILD/web-console.log" 2>&1; then
        note "$(cat "$BUILD/web-console.log")"
    else
        cat "$BUILD/web-console.log"
        fail "web (console.html does not match src/tools/web/client/console)"
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
    # NOT IN THE DEFAULT STAGE LIST, and that is the whole of the gating now.
    # It is 2.5 minutes and it was being re-run by parallel agents told to
    # "run the full set once at the end", so it costs an explicit ask: either
    # name the stage or set AMINETXDUO_ANALYZE=1, and the main loop adds it to
    # the default list for the second.
    #
    # IT USED TO REFUSE HERE AS WELL, and that made both forms of the ask
    # do nothing.  `tools/ci.sh analyze` named the stage and got a skip
    # because the variable was unset; `AMINETXDUO_ANALYZE=1 tools/ci.sh` set
    # the variable and got a default list the stage is not in, and printed
    # "analyze NOT RUN" and "all green" in the same summary.  A gate that
    # answers an explicit request by doing nothing is worse than no gate.
    #
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

    # TWO MACHINES, ONE BUILD.  An A1200 and an A600, both running the build
    # that ships -- which since AMINETXDUO_CPU=any is -m68000 codegen chosen at
    # run time, so there is no separate 68000 tree to build any more and
    # nothing would be gained by one.  This is a stronger arm than the old
    # pair, not a weaker one: what executes on the 68000 is now the very
    # binary a user installs, rather than a variant nobody ships.
    #
    # The two machines still both matter.  That gap once hid a real defect in
    # this suite: soak_test's starvation floor was a constant tuned against
    # 68020 throughput, so the lowest-priority worker failed it on a 68000
    # while being perfectly healthy.
    if [ ! -d "$BUILD/default" ]; then
        fail "no $BUILD/default, run the cross stage first"
        return 1
    fi

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

    local entry exe timeout dir arm cpuopt tag budget
    dir=default
    for arm in a1200 a600; do
        # These budgets multiply the per-test ceilings below.  They are ceilings,
        # not fixed waits, so a passing run finishes long before them and paying
        # for headroom is free, and the hosted GitHub runner is markedly slower
        # at emulation than a dedicated box, enough to time a test out on the old
        # 68020 x1 / 68000 x2 budgets.  Doubled so a slow runner has rope.
        if [ "$arm" = "a600" ]; then
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

# ------------------------------------------------------ release e2e, cards ----
#
# THE SAME GATE, ON EVERY CARD.  stage_e2e above boots the A2065 and nothing
# else, and did so silently: run-workbench.sh staged the driver by name and
# wrote two literal a2065 lines into the emulator config, so every release this
# project has cut was gated on one network card.
#
# Its own stage rather than a loop inside stage_e2e, because the two prove
# different things and need different ingredients.  `e2e` needs a peer and
# drives the machine from outside it; this one needs the vendor drivers in the
# asset store and asks a narrower question of nine machines: does the archive
# install, does the power cycle bring the network up on THIS card.
stage_e2ecards() {
    hr "release end-to-end on every card (tier 2, needs a licensed Workbench)"

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

    "$ROOT/install/test/run-workbench-cards.sh" -a "$archive" || rc=$?

    case "$rc" in
        0) note "PASS  the shipped archive installs and boots on every card" ;;
        2) fail "release e2e cards: no card could run on this machine" ;;
        *) fail "release e2e cards: exit $rc" ;;
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
        return "$NOTHING"
    fi

    "$ROOT/tests/tools/run-cardsweep.sh" -b "$BUILD/default" || rc=$?

    # Two red codes, because there are two claims: 1 is the CARD -- online, and
    # bytes both ways agreeing with a machine off this box -- and 4 is a card
    # that carried the bytes and failed an assertion around them.  4 used to be
    # inside 3, and 3 is a skip, so xsurf100z3 failed for weeks reported as a
    # skipped tier and only went red because ne2000_pcmcia failed beside it.
    #
    # 3 now means nothing failed: a rig that refused an arm, a driver that is
    # not in the store.  Neither is a verdict on a card.
    case "$rc" in
        0) note "PASS  every card came online and carried bytes both ways" ;;
        1) fail "card sweep: a card did not carry traffic -- the table above" \
                "names it and says which direction failed" ;;
        2) fail "card sweep: the rig refused it before any card was measured" ;;
        3) skip "card sweep: every card that was measured passed, and a card" \
                "was not measured -- the table above says which and why" ;;
        4) fail "card sweep: a card carried bytes both ways and an assertion" \
                "in its arm failed -- the table above names it" ;;
        *) fail "card sweep: exit $rc" ;;
    esac
    return "$rc"
}

# --------------------------------------------------------------- capture ----
#
# NetCapture, on every card, reading traffic a DIFFERENT command made, with
# the file read by tcpdump on another machine.
#
# NOT in the default set and not in `emulator`, for the reason `cards` is not:
# it needs a BRIDGE.  Under SLIRP there is no segment, so an unfiltered
# capture sees only the guest's own NAT'd traffic and the filtered/unfiltered
# comparison that proves the filter works is vacuous.  Naming it runs it.
#
# The peer is optional and the run says which it used.  Without one the files
# are read by the tcpdump on this host, which is a weaker claim -- the machine
# that wrote the file is also the one saying it is readable.
stage_capture() {
    hr "NetCapture (tier 2, needs a bridge)"

    local rc=0
    local peer="${AMINETXDUO_NETCAPTURE_PEER:-${AMINETXDUO_PEER:-}}"
    local args=(-b "$BUILD/default" -B "${AMINETXDUO_AMIBERRY_BACKEND:-ens18}")

    [ -z "$peer" ] || args+=(-P "$peer")
    [ -z "${AMINETXDUO_NETCAPTURE_ADDRESS:-}" ] ||
        args+=(-A "$AMINETXDUO_NETCAPTURE_ADDRESS")

    [ -n "$peer" ] ||
        skip "capture: neither AMINETXDUO_NETCAPTURE_PEER nor AMINETXDUO_PEER" \
             "is set, so the pcap files are read by the machine that wrote" \
             "them.  That they open on another machine is unproven here."

    "$ROOT/tests/tools/run-netcapture.sh" "${args[@]}" || rc=$?

    case "$rc" in
        0) note "PASS  every card captured another program's traffic, the" \
                "filter held, the count limit stopped it, and tcpdump read" \
                "every file" ;;
        1) fail "capture: an assertion failed -- the card_ line above names" \
                "the card and the card_*_why line says which" ;;
        2) fail "capture: the rig refused it before any card was booted" ;;
        3) skip "capture: a card produced no file to read -- the card_ line" \
                "above says which, and a driver that is not on this machine" \
                "is one of the reasons" ;;
        *) fail "capture: exit $rc" ;;
    esac
    return "$rc"
}

# ------------------------------------------------------------- wirequiet ----
#
# What the machine EMITS.  Every other stage in this file reads the guest's
# own transcript, and a guest shouting at a router says nothing about it in
# its transcript: a DHCPv6 client that rebound its lease every 120 ms for
# 305 s straight passed all of them, and was found by a person running tcpdump
# by hand.
#
# NOT in the default set, for the reason `capture` is not: it needs a BRIDGE.
# SLIRP has no segment, no DHCPv6 server and nothing for an idle machine to be
# noisy at, so the whole measurement would be of an empty NAT.
stage_wirequiet() {
    hr "the wire, when the machine is idle (tier 2, needs a bridge)"

    local rc=0

    "$ROOT/tests/tools/run-wirequiet.sh" -b "$BUILD/default" \
        -B "${AMINETXDUO_AMIBERRY_BACKEND:-ens18}" || rc=$?

    case "$rc" in
        0) note "PASS  every card went quiet after bring-up and was still" \
                "answering when the window closed" ;;
        1) fail "wirequiet: a card kept talking with nothing asked of it --" \
                "the card_ line above has the count and the breakdown" ;;
        2) fail "wirequiet: the rig refused it before any card was booted" ;;
        3) fail "wirequiet: a card produced no capture or no transcript, so" \
                "the window is not a measurement of anything" ;;
        *) fail "wirequiet: exit $rc" ;;
    esac
    return "$rc"
}

# ---------------------------------------------------------- reachability ----
#
# The machine is inside a certificate chain for a minute and a half.  Is it
# still on the network while it is?  It was not: the peer's neighbour entry
# went FAILED and stayed there about 44 s, and everything that wanted to reach
# the guest got EHOSTUNREACH.  What fixes it is the stride of a yield hook,
# which is a number somebody will change.
#
# Needs a SECOND MACHINE.  Amiberry injects with pcap and injected frames
# never enter this host's own receive path, so a probe from here would report
# the guest unreachable with the fix in or out.
stage_reachability() {
    hr "reachable during a TLS handshake (tier 2, needs a bridge and a peer)"

    local rc=0
    local peer="${AMINETXDUO_REACH_PEER:-${AMINETXDUO_PEER:-}}"

    if [ -z "$peer" ]; then
        skip "reachability: neither AMINETXDUO_REACH_PEER nor AMINETXDUO_PEER" \
             "is set, so there is no machine that can see the guest answer." \
             "Whether a handshake takes it off the network is untested here."
        return "$NOTHING"
    fi

    "$ROOT/tests/tls/run-reachability.sh" -b "$BUILD/default" -P "$peer" \
        -B "${AMINETXDUO_AMIBERRY_BACKEND:-ens18}" || rc=$?

    case "$rc" in
        0) note "PASS  the machine answered throughout the handshake and the" \
                "peer's neighbour entry never failed" ;;
        1) fail "reachability: the machine stopped answering while it was" \
                "doing crypto -- max_gap_s above is how long for" ;;
        2) fail "reachability: the rig refused it, or the guest was already" \
                "unreachable before the handshake started" ;;
        3) fail "reachability: the run produced no handshake to measure" ;;
        *) fail "reachability: exit $rc" ;;
    esac
    return "$rc"
}

# ---------------------------------------------------------------- cards6 ----
#
# EVERY network card again, and off-LAN IPv6 this time: a global address off
# the wire, and an answer from a host past the default router.
#
# `cards` cannot see this and neither could anything else.  It is IPv4 and one
# segment; the tests/ipv6 runners are WinUAE or SLIRP, and SLIRP answers a
# router solicitation itself.  x-surf.device and x-surf-100.device refused
# every S2_ADDMULTICASTADDRESS, so the router's neighbour solicitation for the
# guest was dropped by the card and every off-link answer died one hop away --
# three shipping cards with no off-LAN IPv6 for as long as they were
# supported, and no harness that could go red for it.
#
# Needs a bridge and a LAN with IPv6 on it, and no peer at all: nothing has to
# call into the guest.  Nightly, like `cards`: nine guests, boot to verdict.
stage_cards6() {
    hr "network cards, off-LAN IPv6 (tier 2, needs a bridge with IPv6)"

    local rc=0

    "$ROOT/tests/tools/run-cardsweep6.sh" -b "$BUILD/default" || rc=$?

    # 2 is the lab, not the cards: no IPv6 default route, no global address,
    # a target that is on-link here, an uplink that is down.  The delegation
    # on this LAN has gone away for an afternoon before, and nine red cards
    # would be the wrong reading of it.
    case "$rc" in
        0) note "PASS  every card formed a global address and reached past the router" ;;
        1) fail "off-LAN IPv6: a card cannot reach past the router -- the table" \
                "above names it and says how far it got" ;;
        2) skip "off-LAN IPv6: the lab could not run it, and no card was" \
                "measured -- the error line above says what was missing" ;;
        3) skip "off-LAN IPv6: nothing failed, and a card was not measured --" \
                "no driver in the store, or an interface that never came" \
                "online, which is the card sweep's gate and not this one" ;;
        *) fail "off-LAN IPv6: exit $rc" ;;
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

    # Here rather than in the emulator tier because it is bridged only and
    # that tier runs on SLIRP.  It needs no peer and puts no traffic on the
    # link: the bridge is there because a result taken over slirp says nothing
    # about the stack, and every assertion is on what the guest printed.
    printf '\n-- ConfigureNetInterface on an interface that has only IPv6\n'
    rc=0
    "$ROOT/tests/tools/run-ifconfigure6.sh" -b "$BUILD/default" \
        -B "${AMINETXDUO_AMIBERRY_BACKEND:-ens18}" || rc=$?
    case "$rc" in
        0) note "PASS  it came up on IPv6 alone, GATEWAY6 changed the route" \
                "and replaced rather than accumulated, and ADDRESS6 and" \
                "CONFIGURE6 were refused by name having changed nothing" ;;
        2) fail "ifconfigure6: an ingredient is missing, or the guest ran" \
                "fewer commands than the list has" ; bad=1 ;;
        *) fail "ifconfigure6: the transcript above is the whole run" ; bad=1 ;;
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

    # MLD, which is the one gate that cannot be run anywhere but a real
    # segment: what is being tested is whether a report reaches the wire, and
    # under SLIRP there is no wire, no snooping switch and no second listener.
    printf '\n-- the groups this host listens to, announced or not\n'
    if [ -z "${AMINETXDUO_PEER:-}" ]; then
        skip "mld: AMINETXDUO_PEER is not set, so there is no querier and no" \
             "second listener.  Join and leave would still be checked;" \
             "answering a query and being suppressed would not."
    else
        rc=0
        "$ROOT/tests/ipv6/run-mld.sh" -b "$BUILD/default" \
            -B "${AMINETXDUO_AMIBERRY_BACKEND:-ens18}" \
            -P "$AMINETXDUO_PEER" || rc=$?
        case "$rc" in
            0) note "PASS  reported on join, answered the query in the" \
                    "version it was asked in, stayed quiet when another" \
                    "host answered first, and gave the group back" ;;
            2) fail "mld: an ingredient is missing -- most likely" \
                    "python3-cap in the peer's home directory, with" \
                    "CAP_NET_RAW set on it" ; bad=1 ;;
            3) fail "mld: the run produced no capture to read" ; bad=1 ;;
            *) fail "mld: read the failed= line above" ; bad=1 ;;
        esac
    fi

    # DHCPv6, which needs a real server rather than a real segment: nothing
    # in this tree implements one, and stubbing the thing under test would
    # answer only that the stub and the client agree.  Two arms, because the
    # two ways an interface can come to ask are different code: CONFIGURE6 =
    # DHCP asks outright, CONFIGURE6 = AUTO asks only because the router
    # advertisement on this link sets the M bit, and the second is the one
    # that makes an IPv6-only machine work with nobody editing a file.
    printf '\n-- an IPv6-only machine, addressed by DHCPv6\n'
    if [ -z "${AMINETXDUO_DHCPV6_PEER:-}" ]; then
        skip "dhcpv6: AMINETXDUO_DHCPV6_PEER is not set, so there is no" \
             "DHCPv6 server to answer the guest.  Neither the stateful path" \
             "nor the renewal is proven on this runner."
    else
        for arm in dhcp auto; do
            rc=0
            if [ "$arm" = dhcp ]; then
                "$ROOT/tests/ipv6/run-dhcpv6.sh" -b "$BUILD/default" \
                    -B "${AMINETXDUO_AMIBERRY_BACKEND:-ens18}" \
                    -P "$AMINETXDUO_DHCPV6_PEER" -a dhcp -l 120 -R || rc=$?
            else
                "$ROOT/tests/ipv6/run-dhcpv6.sh" -b "$BUILD/default" \
                    -B "${AMINETXDUO_AMIBERRY_BACKEND:-ens18}" \
                    -P "$AMINETXDUO_DHCPV6_PEER" -a auto || rc=$?
            fi
            case "$rc" in
                0) note "PASS  dhcpv6 $arm: addressed, reachable, renewed" \
                        "and released, with no IPv4 anywhere on the machine" ;;
                2) fail "dhcpv6 $arm: an ingredient is missing -- most" \
                        "likely anxd-dhcpv6-server.sh in the peer's home" \
                        "directory" ; bad=1 ;;
                3) fail "dhcpv6 $arm: the guest never ran" ; bad=1 ;;
                4) skip "dhcpv6 $arm: the link or the peer is not what this" \
                        "needs -- read the reason= above.  The common one is" \
                        "another_dhcpv6_server_answered_first: this link has" \
                        "a second DHCPv6 server on it, the site router, and" \
                        "which of the two the guest takes is a race no" \
                        "server here can settle.  It is not a product" \
                        "failure and must not be turned into one" ;;
                *) fail "dhcpv6 $arm: read the FAIL lines above" ; bad=1 ;;
            esac
        done
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
        return "$NOTHING"
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
        return "$NOTHING"
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
# analyze and sanitize are NOT in the default set. analyze is 2.5 minutes of
# the roughly four a default run takes, and its findings do not move between
# commits the way a build break does. THERE ARE TWO WAYS TO ASK FOR EITHER,
# AND BOTH NOW RUN IT:
#
#     tools/ci.sh analyze                  name the stage
#     AMINETXDUO_ANALYZE=1 tools/ci.sh     set the variable; the default list
#                                          below gains the stage
#
# Both used to be refused -- the stage checked the variable as well as being
# named -- so `tools/ci.sh analyze` skipped and `AMINETXDUO_ANALYZE=1
# tools/ci.sh` printed "analyze NOT RUN" and "all green" together.
#
# The release workflow does both, so nothing ships unanalysed. A default run
# says out loud that it skipped, because a stage that goes quiet reads as
# coverage it is not providing.
#
WANT=("$@")
if [ ${#WANT[@]} -eq 0 ]; then
    WANT=(host host32 cross web conformance)
    # THE VARIABLE IS THE ASK.  Setting it and getting a run that prints
    # "analyze NOT RUN" is the mechanism behind every false green report this
    # gate has produced; the variable was necessary and not sufficient, and
    # the stage had to be named as well.  Naming it still works and is still
    # what CI does.  Same for sanitize, which was gated the same way.
    if [ "${AMINETXDUO_SANITIZE:-0}" = "1" ]; then WANT+=(sanitize); fi
    if [ "${AMINETXDUO_ANALYZE:-0}"  = "1" ]; then WANT+=(analyze);  fi
fi

stage_submodules

# Anything but a pure host run needs the cross compiler.
for s in "${WANT[@]}"; do
    case "$s" in
        cross|analyze|conformance|emulator|e2e|e2ecards|cards|cards6|capture|wirequiet|reachability|bridged|lossgate|smb)
            stage_toolchain; break ;;
    esac
done

# How many stages produced a verdict, in either direction.  A stage that
# returns $NOTHING did not: it reached its own gate and stopped.  Counted
# rather than inferred from SKIPPED, which also collects the HALVES a stage
# skipped while running the rest, and those are a partial run and not this.
STAGES_TESTED=0

for s in "${WANT[@]}"; do
    srrc=0
    case "$s" in
        toolchain)   [ -n "${AMIGA_TOOLCHAIN_ROOT:-}" ] || stage_toolchain ;;
        host)        stage_host || srrc=$? ;;
        host32)      stage_host32 || srrc=$? ;;
        sanitize)    stage_sanitize || srrc=$? ;;
        # `|| srrc=$?` on every stage, cross included: bash suppresses `set -e`
        # inside a function called that way, which is what keeps one unguarded
        # command in a stage from taking the summary down with it.  It used to
        # be `|| true`, which threw the one thing the caller needed away.
        cross)       stage_cross || srrc=$? ;;
        web)         stage_web || srrc=$? ;;
        analyze)     stage_analyze || srrc=$? ;;
        conformance) stage_conformance || srrc=$? ;;
        emulator)    stage_emulator || srrc=$? ;;
        cards)       stage_cards || srrc=$? ;;
        cards6)      stage_cards6 || srrc=$? ;;
        capture)     stage_capture || srrc=$? ;;
        wirequiet)   stage_wirequiet || srrc=$? ;;
        reachability) stage_reachability || srrc=$? ;;
        bridged)     stage_bridged || srrc=$? ;;
        lossgate)    stage_lossgate || srrc=$? ;;
        smb)         stage_smb || srrc=$? ;;
        e2e)         stage_e2e || srrc=$? ;;
        e2ecards)    stage_e2ecards || srrc=$? ;;
        *) echo "unknown stage: $s" >&2; exit 2 ;;
    esac
    [ "$srrc" = "$NOTHING" ] || STAGES_TESTED=$((STAGES_TESTED + 1))
    STAGES_RUN+=("$s")
done

hr "summary"
# What was built, in the same words CI names its artefacts with.  This also
# re-checks the recorded NetX Duo/ThreadX versions against third_party/.
note "version: $(tools/version.sh --long 2>&1 || echo 'version.sh FAILED -- see above')"
note "stages: ${STAGES_RUN[*]}"

case " ${STAGES_RUN[*]} " in
    *" analyze "*) ;;
    *) note "analyze NOT RUN, AMINETXDUO_ANALYZE=1 tools/ci.sh" ;;
esac
case " ${STAGES_RUN[*]} " in
    *" sanitize "*) ;;
    *) note "sanitize NOT RUN, AMINETXDUO_SANITIZE=1 tools/ci.sh" ;;
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
    # Nothing failed AND nothing ran.  Green is the wrong word for it: this is
    # the state a runner is in when it has none of the ingredients, and it read
    # identically to a runner that had them all.  Failures are checked first,
    # so a stage that skipped beside one that failed is still a failure.
    if [ "$STAGES_TESTED" -eq 0 ] && [ ${#STAGES_RUN[@]} -gt 0 ]; then
        printf '\033[33mNOTHING WAS TESTED\033[0m: every stage (%s) skipped;\n' \
               "${STAGES_RUN[*]}"
        printf 'there is no result here, in either direction.\n'
        exit "$NOTHING"
    fi
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
