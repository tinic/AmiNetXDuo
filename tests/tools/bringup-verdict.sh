#!/usr/bin/env bash
#
# Did the machine come up AND carry a packet?  One grader, sourced without an
# emulator.
#
#   . "$ROOT/tests/tools/bringup-verdict.sh"
#   bringup_verdict <netstack_test-transcript> [min-pool-packets]
#
# WHY IT IS NOT tools/test-verdict.sh
#
#   That one reads the guest's own summary -- `N checks, M failures` -- and a
#   floor under N.  It is the right gate for a harness whose question is "did
#   the test run", and it is the wrong one for a MATRIX arm, whose question is
#   "did this MACHINE reach the network".  The difference is that the checks
#   netstack_test runs are conditional: `ICMP echo to gateway` is inside
#   `if (gateway != 0UL)` (tests/netstack/netstack_test.c:394), so a guest that
#   never got a lease runs one check FEWER and can still clear a floor of 12.
#   The arm that would have caught a CPU-speed-dependent delay is exactly the
#   arm where the lease does not arrive, so a count is the one thing that
#   cannot be the assertion here.
#
#   So this asserts on the checks BY NAME, out of the transcript's own
#   `  ok   <what>` and `  FAIL <what> (0x...)` lines
#   (tests/netstack/netstack_test.c:128-137).  A check that stops being run at
#   all is absent, which is a failure and not a pass -- the distinction the
#   floor exists for, made per assertion instead of in aggregate.
#
# THE THREE FACTS, and why each one is separate
#
#   link      `interface 0 link is up`.  The driver opened and the card
#             answered.  A collapsed settle loop can still get this far: the
#             claim is a register write, not a timed handshake.
#   address   `interface 0 has an address`.  DHCP completed, which is frames
#             out and frames in, on a timer.
#   traffic   `ICMP echo to gateway`.  Something that is not this machine
#             answered.  This is the one that goes red when a delay calibrated
#             for a 14 MHz 68020 runs on a PiStorm: the card is claimed, the
#             address may even arrive, and the wire does not work.
#
#   Reported and asserted one at a time because WHICH of them broke is the
#   whole diagnosis, and a single pass/fail throws it away.
#
# POOL SIZING is read out of `  pool %ld packets (%ld free) of %ld bytes`
# (netstack_test.c:351) and gated only when a caller passes a floor.  It is the
# arithmetic tests/tools/run-bigmem.sh exists for: AvailMem()/16 with clamps,
# which has never run on a machine with 32 MB in it.
#
# Output is key=value on stdout and the return code is the verdict, the same
# interface tools/test-verdict.sh publishes.
#
# SPDX-License-Identifier: MIT

# The transcript line for one named check, or nothing.  `ok` and `FAIL` are
# both matched so that a check that RAN and failed is distinguishable from one
# that was never reached; the two are different defects and the caller says so.
_bv_line() { # transcript what
    grep -aF -- "$2" "$1" 2>/dev/null | grep -aE '^[[:space:]]+(ok|FAIL)[[:space:]]' | tail -1
}

# bringup_verdict TRANSCRIPT [MIN_POOL_PACKETS] -> 0 pass, 1 fail
bringup_verdict() {
    local report="$1" poolmin="${2:-0}"
    local failed=0 line what state
    local packets="" bytes=""

    _bv_fail() { echo "bringup_fail=$1"; failed=1; }

    if [ ! -s "$report" ]; then
        echo "bringup_report=missing"
        _bv_fail no_transcript
        echo "bringup_result=FAIL"
        return 1
    fi
    echo "bringup_report=present"

    # THE ORDER IS THE STORY: link, then address, then a packet that came back.
    # Read them all rather than stopping at the first, because "link up, no
    # address" and "address, no answer" are different findings and a reader
    # who gets only the first one guesses at the rest.
    for what in "interface 0 link is up" \
                "interface 0 has an address" \
                "ICMP echo to gateway"
    do
        line=$(_bv_line "$report" "$what")
        state=absent
        case "$line" in
            *"  ok   "*)  state=ok ;;
            *"FAIL"*)     state=failed ;;
        esac

        # The key is the check's name with the spaces taken out, so a reader
        # greps the transcript for the same words the key is made of.
        printf 'bringup_%s=%s\n' "$(printf '%s' "$what" | tr ' ' '_')" "$state"

        case "$state" in
            ok) ;;
            failed)
                # The guest ran the check and it came back false.  Its own
                # detail word is in the line; carry it, it is the error code.
                _bv_fail "$(printf '%s' "$what" | tr ' ' '_')_failed" ;;
            absent)
                # Never reached.  On this test that means the run stopped
                # earlier -- a conditional check whose condition was false, or
                # a guest that did not get that far at all.
                _bv_fail "$(printf '%s' "$what" | tr ' ' '_')_never_ran" ;;
        esac
    done

    # `pool 84 packets (84 free) of 1592 bytes`
    line=$(grep -aoE 'pool [0-9]+ packets \([0-9]+ free\) of [0-9]+ bytes' \
                "$report" 2>/dev/null | tail -1)
    if [ -n "$line" ]; then
        packets=$(printf '%s\n' "$line" | awk '{print $2}')
        bytes=$(printf '%s\n' "$line" | awk '{print $(NF - 1)}')
    fi
    echo "bringup_pool_packets=${packets:-none}"
    echo "bringup_pool_bytes=${bytes:-none}"

    if [ "$poolmin" != 0 ]; then
        if [ -z "$packets" ]; then
            _bv_fail pool_line_absent
        elif [ "$packets" -lt "$poolmin" ]; then
            _bv_fail "pool_${packets}_below_${poolmin}"
        fi
    fi

    if [ "$failed" = 0 ]; then
        echo "bringup_result=PASS"
        return 0
    fi
    echo "bringup_result=FAIL"
    return 1
}
