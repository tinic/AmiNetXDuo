#!/usr/bin/env bash
#
# Run a guest that has to reach a peer on THIS host.
#
#   tools/emu-net-run.sh -n [-m MODEL] [-t SECS] [-c CPU] [-a ARGS]
#                        <executable> [extra files...]
#
# Takes tools/amiberry-run.sh's arguments unchanged, so a caller switches to it
# by changing the script name and nothing else.
#
# WHY THIS EXISTS
#
#   The Debian fs-uae 3.1.66 imports SLIRP from the qemu-uae plugin and, with
#   the plugin absent, logs
#
#, stub, uae_slirp_start
#
#   and carries on.  The A2065 initialises, the link comes up, DHCP is
#   answered inside the stub, and there is no NAT behind any of it, so every
#   TCP connection to 10.0.2.2 is refused and the host peer never sees one.
#   A workload measuring the wire then reports a timeout that looks exactly
#   like a defect in the stack.  That is how tests/compare/run-compare.sh and
#   tests/trace/run-trace.sh spent their wire arms measuring nothing:
#   `wire.pcap` holds 11 packets and no TCP flow, and the peer log holds its
#   own startup lines and nothing else.
#
#   Amiberry has SLIRP compiled in.  The same plan through it: wire 64 ticks
#   against fs-uae's 6,368-tick timeout, ping 229 against 825.
#
#   fs-uae is gone entirely, 2026-08-04.  It needed an X server and its SLIRP
#   was a stub, so both halves of what this script exists to pick were bad.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

NETWORK=0
MODEL=""
TIMEOUT=""
CPU=""
GUEST_ARGS=""

CLOCK=""
while getopts "nm:t:c:a:k:" opt; do
    case "$opt" in
        n) NETWORK=1 ;;
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        c) CPU="$OPTARG" ;;
        k) CLOCK="$OPTARG" ;;
        a) GUEST_ARGS="$OPTARG" ;;
        *) sed -n '3,30p' "$0" >&2; exit 2 ;;
    esac
done
shift $((OPTIND - 1))

[ $# -ge 1 ] || { sed -n '3,30p' "$0" >&2; exit 2; }

EMU="${AMINETXDUO_EMULATOR:-amiberry}"

case "$EMU" in
    amiberry)
        ARGS=()
        # -n is "attach the card"; on Amiberry that is the board and the
        # backend, and SLIRP is the backend every harness here assumes.
        [ "$NETWORK" = "1" ] && ARGS+=(-N "${AMINETXDUO_EMU_BOARD:-a2065}"
                                       -B "${AMINETXDUO_EMU_BACKEND:-slirp}")
        [ -n "$MODEL" ]      && ARGS+=(-m "$MODEL")
        [ -n "$TIMEOUT" ]    && ARGS+=(-t "$TIMEOUT")
        [ -n "$CPU" ]        && ARGS+=(-c "$CPU")
        [ -n "$CLOCK" ]      && ARGS+=(-k "$CLOCK")
        [ -n "$GUEST_ARGS" ] && ARGS+=(-a "$GUEST_ARGS")

        set +e
        "$ROOT/tools/amiberry-run.sh" "${ARGS[@]}" "$@"
        RC=$?
        set -e

        # The two emulators write the guest disk to different names --
        # build/testhd-<tag> and build/amiberry-testhd-<tag>, and every
        # caller of this script reads results out of the first.  Link the
        # second onto it rather than teach each of them both, so a harness
        # that finds no results means the run produced none.
        TAG="${AMINETXDUO_RUN_TAG:-}"
        if [ -n "$TAG" ] && [ -d "$ROOT/build/amiberry-testhd-$TAG" ]; then
            if [ ! -e "$ROOT/build/testhd-$TAG" ] ||
               [ -L "$ROOT/build/testhd-$TAG" ]; then
                ln -sfn "amiberry-testhd-$TAG" "$ROOT/build/testhd-$TAG"
            fi
        fi
        exit $RC
        ;;
    *)
        echo "AMINETXDUO_EMULATOR: only amiberry is supported now, got '$EMU'" >&2
        echo "  fs-uae is gone: it needs an X server, and the 3.1.66 SLIRP is a" >&2
        echo "  stub that logs 'stub, uae_slirp_start' and carries on, so a" >&2
        echo "  networked run measured nothing." >&2
        exit 2
        ;;
esac
