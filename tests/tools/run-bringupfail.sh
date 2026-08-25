#!/usr/bin/env bash
#
# WHAT A USER IS TOLD WHEN BRING-UP FAILS, for every cause that can be reached.
#
#   tests/tools/run-bringupfail.sh [-b BUILDDIR] [-t SECONDS] [-N BOARD]
#
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
"$ROOT/tests/tools/check-no-log-advice.sh" "$BUILD"
case "$?" in
    0) printf 'strings   PASS  no shipped command sends the user to a log\n' \
           >> "$RESULTS" ;;
    2) printf 'strings   SKIP  nothing built at %s to check\n' "$BUILD" \
           >> "$RESULTS" ;;
    *) printf 'strings   FAIL  a shipped command sends the user to a log\n' \
           >> "$RESULTS"
       FAILED=$((FAILED + 1)) ;;
esac

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

    printf 'DEVICE=nosuchcard.device\nUNIT=0\nCONFIGURE=DHCP\n' \
        > "$STAGE/devs/NetInterfaces/nodev"
    printf 'DEVICE=a2065.device\nUNIT=9\nCONFIGURE=DHCP\n' \
        > "$STAGE/devs/NetInterfaces/badunit"
    printf 'DEVICE=a2065.device\nUNIT=0\nCONFIGURE=STATIC\nADDRESS=300.1.1.1\nNETMASK=255.255.255.0\n' \
        > "$STAGE/devs/NetInterfaces/badaddr"
    for n in cap0 cap1 cap2 cap3 cap4; do
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
SYS:AddNetInterface cap3
SYS:AddNetInterface cap4
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
attach_cap        cap4
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

if ! grep -q '  PASS  ' "$RESULTS" && ! grep -q '^live      FAIL' "$RESULTS"; then
    echo "bringupfail: SKIPPED -- neither half of this harness could run."
    echo "             Build the tree and set AMINETXDUO_KICKSTART."
    exit 2
fi

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
