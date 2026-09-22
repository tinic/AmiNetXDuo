#!/usr/bin/env bash
#
# A SOFTWARE INTERRUPT THAT Cause()s ITSELF, ON EVERY KICKSTART THE LAB HAS.
#
#   tests/tools/run-selfcause.sh [-b builddir] [-t seconds] [-a arm[,arm]] [-l]
#
# The netdev bottom half (netdev_soft, src/netdev/netdev_device.c) is an Exec
# software interrupt, and the driver's design assumes what the V40 source
# reads: the dispatcher sets ln_Type back to NT_INTERRUPT BEFORE it calls
# is_Code, so a Cause() from inside the handler re-queues it and it runs again.
# tests/tools/selfcause.c asks the ROM, and this boots it on both floors this
# tree supports -- Kickstart 2.x (exec V37) and 3.1 (V40) -- plus 3.0 (V39)
# where that ROM is present.  No card, no network, no stack: exec and dos only.
#
# Each arm prints what the guest printed, as key=value, and the gate is
#
#   selfcause_runs=2                 the re-Cause was queued and ran
#   selfcause_pending_suppressed=1   two Cause() on a PENDING interrupt ran
#                                    it once -- the documented rule, and the
#                                    control that the rig can count at all
#
# Exit 0 only when every required floor (V37 and V40) passed both.  A required
# ROM that is not on this machine is a refusal (exit 2) that names the file,
# never a silent skip; the V39 arm is the one that may be skipped, by name.
#
# SPDX-License-Identifier: MIT

set -uo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT" || exit 2

BUILD="${AMINETXDUO_BUILD:-build/cm}"
TIMEOUT=120
ONLY=""
LIST=0

while getopts "b:t:a:l" opt; do
    case "$opt" in
        b) BUILD="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        a) ONLY="$OPTARG" ;;
        l) LIST=1 ;;
        *) sed -n '3,5p' "$0" >&2; exit 2 ;;
    esac
done

case "$BUILD" in
    /*) ;;
    *)  BUILD="$ROOT/$BUILD" ;;
esac

# arm        model  cpu    rom   required
#
# v37 and v40 are the two floors and both are required.  The A2000 pair is the
# SAME 68000 machine on 2.04 and on 3.1, so a difference between those two
# rows is the ROM and nothing else; the A1200 pair is the same on 3.0 and 3.1.
# v37-a600 is 2.05, the other V37 exec (37.350 against 37.175).
ARMS="
v37-a2000  A2000  68000  v204  yes
v37-a600   A600   68000  v205  no
v39-a1200  A1200  68020  v39   no
v40-a2000  A2000  68000  v31   yes
v40-a1200  A1200  68020  v40   yes
"

# The ROM files, named the way tests/tools/run-kick2x.sh names them, with the
# same environment overrides.  v40 is the lab's default AMINETXDUO_KICKSTART.
rom_for() { # v204|v205|v39|v31|v40
    case "$1" in
    v204) printf '%s\n' "${AMINETXDUO_KICKSTART_V204:-\
${AMINETXDUO_KICKSTART_A2000:-\
$HOME/amiga-assets/roms/Kickstart v2.04 r37.175 (1991)(Commodore)(A500+)[!].rom}}" ;;
    v205) printf '%s\n' "${AMINETXDUO_KICKSTART_V205:-\
$HOME/amiga-assets/roms/Kickstart v2.05 r37.350 (1992)(Commodore)(A600HD)[!].rom}" ;;
    v39)  printf '%s\n' "${AMINETXDUO_KICKSTART_V39:-\
$HOME/amiga-assets/roms/Kickstart v3.0 r39.106 (1992)(Commodore)(A1200)[!].rom}" ;;
    v31)  printf '%s\n' "${AMINETXDUO_KICKSTART_V31:-\
$HOME/amiga-assets/roms/Kickstart v3.1 r40.63 (1993)(Commodore)(A500-A600-A2000)[!].rom}" ;;
    v40)  printf '%s\n' "${AMINETXDUO_KICKSTART:-\
$HOME/amiga-assets/roms/Kickstart v3.1 r40.68 (1993)(Commodore)(A1200)[!].rom}" ;;
    esac
}

selected() { # arm
    case ",$ONLY," in
        ,,) return 0 ;;
        *",$1,"*) return 0 ;;
        *) return 1 ;;
    esac
}

if [ "$LIST" = 1 ]; then
    printf '%-10s %-6s %-6s %-5s %s\n' arm model cpu rom required
    printf '%s\n' "$ARMS" | while read -r a m c r q; do
        [ -n "$a" ] || continue
        selected "$a" || continue
        printf '%-10s %-6s %-6s %-5s %s\n' "$a" "$m" "$c" "$r" "$q"
    done
    exit 0
fi

PROBE="$BUILD/tests/tools/SelfCause"
[ -f "$PROBE" ] || {
    echo "selfcause_stage=missing:$PROBE" >&2
    echo "build the cross tree first (target test_selfcause)" >&2
    exit 2; }

RESULTS="$ROOT/build/selfcause-results.txt"
: > "$RESULTS"
LOGDIR="$ROOT/build/selfcause-logs"
rm -rf "$LOGDIR"; mkdir -p "$LOGDIR"

COUNT=0; FAILED=0; SKIPPED=0; MISSING=0

# One key out of the guest's transcript, or "-" when it never printed it.
key_of() { # file key
    sed -n "s/^$2=//p" "$1" 2>/dev/null | tail -1 | tr -d '\r' | sed 's/ *$//'
}

run_arm() { # arm model cpu rom required
    local arm="$1" model="$2" cpu="$3" rom="$4" required="$5"
    local tag="selfcause-$arm" rom_path rc report started kickvar
    local runs pend supp verdict exec_id exec_ver rom_ver

    rom_path=$(rom_for "$rom")
    if [ ! -f "$rom_path" ]; then
        if [ "$required" = yes ]; then
            printf 'arm=%s rom=%s status=missing_rom file="%s"\n' \
                   "$arm" "$rom" "$rom_path" | tee -a "$RESULTS"
            MISSING=$((MISSING + 1))
        else
            printf 'arm=%s rom=%s status=skip reason="no ROM at %s"\n' \
                   "$arm" "$rom" "$rom_path" | tee -a "$RESULTS"
            SKIPPED=$((SKIPPED + 1))
        fi
        return 0
    fi

    # AMINETXDUO_KICKSTART_<MODEL> is what tools/amiberry-run.sh reads first,
    # set for this arm alone so the lab's env.sh cannot put a 3.1 ROM under
    # the 2.x arm.
    kickvar="AMINETXDUO_KICKSTART_$model"

    echo
    echo "=============================================================="
    echo "==> $arm: $model at $cpu, Kickstart $rom, ${TIMEOUT}s"
    echo "    $(basename "$rom_path")"
    echo "=============================================================="

    started=$(date +%s)
    (
        export AMINETXDUO_RUN_TAG="$tag"
        export "$kickvar=$rom_path"
        "$ROOT/tools/amiberry-run.sh" -m "$model" -c "$cpu" -t "$TIMEOUT" \
            "$PROBE"
    ) > "$LOGDIR/$arm.log" 2>&1
    rc=$?

    report="$ROOT/build/amiberry-testhd-$tag/stdout.txt"
    if [ ! -f "$report" ]; then
        tail -20 "$LOGDIR/$arm.log"
        printf 'arm=%s rom=%s verdict=FAIL reason=no_transcript run_rc=%s log=%s\n' \
               "$arm" "$rom" "$rc" "$LOGDIR/$arm.log" | tee -a "$RESULTS"
        FAILED=$((FAILED + 1)); COUNT=$((COUNT + 1))
        return 0
    fi

    sed 's/^/    /' "$report"

    exec_id=$(key_of "$report" exec_id)
    exec_ver=$(key_of "$report" exec_version)
    rom_ver=$(key_of "$report" kickstart_rom)
    runs=$(key_of "$report" selfcause_runs)
    pend=$(key_of "$report" selfcause_pending_runs)
    supp=$(key_of "$report" selfcause_pending_suppressed)

    verdict=FAIL
    if [ "$runs" = 2 ] && [ "$supp" = 1 ] && [ "$rc" = 0 ]; then
        verdict=PASS
    else
        FAILED=$((FAILED + 1))
    fi

    printf 'arm=%s model=%s cpu=%s rom=%s kickstart=%s exec_version=%s exec_id="%s" selfcause_runs=%s selfcause_pending_runs=%s selfcause_pending_suppressed=%s verdict=%s run_rc=%s wall_s=%s\n' \
           "$arm" "$model" "$cpu" "$rom" "${rom_ver:--}" "${exec_ver:--}" \
           "$exec_id" "${runs:--}" "${pend:--}" "${supp:--}" "$verdict" \
           "$rc" "$(( $(date +%s) - started ))" | tee -a "$RESULTS"
    COUNT=$((COUNT + 1))
    return 0
}

while read -r arm model cpu rom required; do
    [ -n "$arm" ] || continue
    selected "$arm" || continue
    run_arm "$arm" "$model" "$cpu" "$rom" "$required"
done <<EOF
$ARMS
EOF

echo
echo "=========================== SelfCause ==========================="
cat "$RESULTS"
echo "================================================================="
echo "selfcause_arms=$COUNT selfcause_failed=$FAILED selfcause_skipped=$SKIPPED\
 selfcause_missing_rom=$MISSING"

if [ "$MISSING" != 0 ]; then
    echo "result=refused"
    echo "a required Kickstart is not on this machine; the status=missing_rom" >&2
    echo "row above names the file" >&2
    exit 2
fi
[ "$COUNT" -gt 0 ] || { echo "result=refused"; echo "no arm ran" >&2; exit 2; }

if [ "$FAILED" = 0 ]; then
    echo "result=ok"
    exit 0
fi
echo "result=fail"
exit 1
