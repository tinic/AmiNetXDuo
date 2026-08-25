#!/usr/bin/env bash
#
# WHAT A USER IS TOLD WHEN BRING-UP FAILS, for every cause that can be reached.
#
#   tests/tools/run-bringupfail.sh [-b BUILDDIR] [-t SECONDS] [-N BOARD]
#
# EXPECT THIS TO BE RED.  It is written against the contract in
# tests/tools/bringupfail-verdict.sh, and the tree does not meet it yet: three
# shipped commands send the user to a debug log that a shipping build cannot
# write.  A red run here is the finding, not a broken harness.  See
# "WHAT IS RED TODAY" below.
#
# WHAT IT PROVES
#
#   Drive bring-up into each failure a user can actually reach, and read the
#   refusal back:
#
#     missing device     DEVICE names a driver that is not on the machine
#     wrong unit         the right driver, a unit it does not have
#     unusable address   CONFIGURE=STATIC with an ADDRESS that is not one
#     attach cap         more interfaces asked for than the stack can hold
#     no memory          the pool cannot be allocated
#
#   and assert, per cause, that the FIRST line names the failing operation and
#   its code, and that NOTHING anywhere sends the reader to a log.
#
# WHY IT EXISTS
#
#   Several verdict selftests in this tree assert the wording of a SUCCESS.
#   None asserted the wording of a FAILURE, which is the half a user reads --
#   nobody studies the output of a machine that worked.  With nothing grading
#   it, "Check the debug log for what failed" shipped and stayed shipped, in a
#   build configuration that writes no log at all.
#
# WHAT IS RED TODAY
#
#   tests/tools/check-no-log-advice.sh, which this runs FIRST and before any
#   emulator, finds the sentence in three commands:
#   AddNetInterface, CheckNetConfig and Online.  It is one sentence in one
#   shared help block reached from three places, and it fails clause 2 for
#   every cause that ends in "the interface did not come online".
#
#   Clause 1 is graded per cause from the live transcript, and what it says is
#   whatever the run says.  Read the table.
#
# THE STRINGS CHECK RUNS WITHOUT A ROM, on purpose: it is the half of this
# harness that can go red on any machine, and putting it behind five boots
# would have hidden it behind a rig requirement for no reason.
#
# NO MEMORY IS NOT DRIVEN LIVE.  Starving an emulated A1200 to the point where
# the pool allocation fails, without also starving it to the point where the
# Shell cannot load the command, is a narrow window and a flaky arm.  The
# cause is covered by a fixture in tests/tools/bringupfail-verdict-selftest.sh
# and is reported here as `fixture' rather than being silently absent from the
# table.
#
# COST: one boot, about half a minute, plus a strings grep.
#
# SPDX-License-Identifier: MIT

set -uo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT" || exit 2

BUILD="${AMINETXDUO_BUILD:-build/cm}"
BOARD=a2065
TIMEOUT=240

while getopts "b:t:N:" opt; do
    case "$opt" in
        b) BUILD="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        N) BOARD="$OPTARG" ;;
        *) echo "usage: $0 [-b builddir] [-t seconds] [-N board]" >&2; exit 2 ;;
    esac
done

. "$ROOT/tests/tools/bringupfail-verdict.sh"

RESULTS="$ROOT/build/bringupfail-results.txt"
: > "$RESULTS"
FAILED=0

# ------------------------------------------------- the half with no rig ----

echo "=============================================================="
echo "==> shipped commands, and whether any of them names a log"
echo "=============================================================="
if "$ROOT/tests/tools/check-no-log-advice.sh" "$BUILD"; then
    printf 'strings   PASS  no shipped command sends the user to a log\n' >> "$RESULTS"
else
    printf 'strings   FAIL  a shipped command sends the user to a log\n' >> "$RESULTS"
    FAILED=$((FAILED + 1))
fi

# ------------------------------------------------------- the live half ----

BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"
ADDIF="$ROOT/$BUILD/src/tools/AddNetInterface"
SHOW="$ROOT/$BUILD/src/tools/ShowNetStatus"
SMOKE="$ROOT/$BUILD/src/tools/ToolsSmoke"

RIG_OK=1
for f in "$BSD" "$ADDIF" "$SHOW" "$SMOKE"; do
    [ -f "$f" ] || { echo "!! no $f; the live half is skipped" >&2; RIG_OK=0; }
done
[ -n "${AMINETXDUO_KICKSTART:-}" ] || {
    echo "!! no AMINETXDUO_KICKSTART; the live half is skipped" >&2; RIG_OK=0; }

A2065="${AMINETXDUO_A2065:-}"
if [ -z "$A2065" ]; then
    for c in "$ROOT/build/a2065.device" "$HOME/amiga-assets/devs/a2065.device"; do
        [ -f "$c" ] && { A2065="$c"; break; }
    done
fi
[ -n "$A2065" ] && [ -f "$A2065" ] || {
    echo "!! no a2065.device; the live half is skipped" >&2; RIG_OK=0; }

if [ "$RIG_OK" = 1 ]; then
    TAG=matrix-bringupfail
    STAGE="$ROOT/build/bringupfail-stage"
    HD="$ROOT/build/amiberry-testhd-$TAG"
    REPORT="$HD/tools.txt"

    rm -rf "$STAGE"
    mkdir -p "$STAGE/libs" "$STAGE/devs/NetInterfaces"
    cp "$BSD" "$STAGE/libs/bsdsocket.library"
    cp "$A2065" "$STAGE/devs/a2065.device"

    # ONE FILE PER CAUSE, named for the cause, so the transcript reads as the
    # table it is about to become.
    printf 'DEVICE=nosuchcard.device\nUNIT=0\nCONFIGURE=DHCP\n' \
        > "$STAGE/devs/NetInterfaces/nodev"
    printf 'DEVICE=a2065.device\nUNIT=9\nCONFIGURE=DHCP\n' \
        > "$STAGE/devs/NetInterfaces/badunit"
    printf 'DEVICE=a2065.device\nUNIT=0\nCONFIGURE=STATIC\nADDRESS=300.1.1.1\nNETMASK=255.255.255.0\n' \
        > "$STAGE/devs/NetInterfaces/badaddr"
    # Three valid definitions of the one card there is.  The first attaches;
    # by the third the stack has no room, which is the attach cap reached as a
    # configuration rather than as an argument.
    for n in cap0 cap1 cap2; do
        printf 'DEVICE=a2065.device\nUNIT=0\nCONFIGURE=DHCP\n' \
            > "$STAGE/devs/NetInterfaces/$n"
    done

    cat > "$STAGE/commands.txt" <<'EOF'
SYS:AddNetInterface nodev
SYS:AddNetInterface badunit
SYS:AddNetInterface badaddr
SYS:AddNetInterface cap0
SYS:AddNetInterface cap1
SYS:AddNetInterface cap2
SYS:ShowNetStatus INTERFACES
EOF

    echo
    echo "=============================================================="
    echo "==> driving bring-up into each failure a user can reach"
    echo "=============================================================="
    (
        export AMINETXDUO_RUN_TAG="$TAG"
        "$ROOT/tools/amiberry-run.sh" -N "$BOARD" -t "$TIMEOUT" \
            "$SMOKE" "$STAGE/devs" "$STAGE/libs" "$ADDIF" "$SHOW" \
            "$STAGE/commands.txt"
    )
    RUN_RC=$?

    if [ ! -s "$REPORT" ]; then
        echo "!! the guest wrote no $REPORT (run rc=$RUN_RC)" >&2
        printf 'live      FAIL  no transcript (run_rc=%s)\n' "$RUN_RC" >> "$RESULTS"
        FAILED=$((FAILED + 1))
    else
        echo
        echo "---------------- what the guest printed ------------------"
        cat "$REPORT"
        echo "----------------------------------------------------------"
        echo

        # ToolsSmoke writes `===== <command> =====` before each command
        # (src/tools/toolssmoke.c run_command()), which is the only reliable
        # boundary: a refusal is several lines and the blank lines inside it
        # are part of the message.
        cause_slice() { # ifname
            tr -d '\r' < "$REPORT" |
                awk -v want="===== SYS:AddNetInterface $1 =====" '
                    $0 == want { grab = 1; next }
                    /^===== / { grab = 0 }
                    grab { print }'
        }

        T=$(mktemp -d)
        trap 'rm -rf "$T"' EXIT

        while read -r cause ifname; do
            [ -n "$cause" ] || continue
            cause_slice "$ifname" > "$T/$cause"
            echo "--- $cause ($ifname) ---"
            if bringupfail_verdict "$T/$cause" "$cause" AddNetInterface |
               sed 's/^/    /'; then
                printf 'live      PASS  %-18s %s\n' "$cause" "$ifname" >> "$RESULTS"
            else
                printf 'live      FAIL  %-18s %s\n' "$cause" "$ifname" >> "$RESULTS"
                FAILED=$((FAILED + 1))
            fi
        done <<'EOF'
missing_device    nodev
wrong_unit        badunit
unusable_address  badaddr
attach_cap        cap2
EOF
    fi
else
    printf 'live      SKIP  no rig (ROM, driver or build missing)\n' >> "$RESULTS"
fi

printf 'no_memory fixture  driven by tests/tools/bringupfail-verdict-selftest.sh\n' \
    >> "$RESULTS"

echo
echo "=================== the refusal matrix ========================"
cat "$RESULTS"
echo "==============================================================="
echo "bringupfail_failed=$FAILED"

if [ "$FAILED" = 0 ]; then
    echo "bringupfail: PASS -- every refusal names its operation and its code,"
    echo "             and nothing sends the user to a log"
    exit 0
fi

echo
echo "bringupfail: FAIL -- $FAILED assertions" >&2
echo >&2
echo "  THE CONTRACT, in tests/tools/bringupfail-verdict.sh:" >&2
echo >&2
echo "    1. The FIRST line names the failing OPERATION and its CODE." >&2
echo "       'AddNetInterface: could not open a2065.device unit 0 (error 32)'" >&2
echo "       and not 'eth0 did not come online'." >&2
echo >&2
echo "    2. NOTHING anywhere sends the reader to a log.  AMINETXDUO_LOG is" >&2
echo "       off in every shipping drawer, so the log does not exist and the" >&2
echo "       advice cannot be followed." >&2
echo >&2
echo "  tests/tools/bringupfail-verdict-selftest.sh has a worked example of" >&2
echo "  the wording for each of the five causes." >&2
exit 1
