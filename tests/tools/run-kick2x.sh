#!/usr/bin/env bash
#
# THE SAME BRING-UP, ON KICKSTART 2.x.
#
#   tests/tools/run-kick2x.sh [-b builddir] [-t seconds] [-B backend]
#                             [-a arm[,arm]] [-l]
#
# Every other bring-up arm boots Kickstart 3.1, so two things this tree relies
# on have never run under a V37 ROM:
#
#   the romtag   anxnet.device is RTF_AUTOINIT with a struct Resident
#                (src/netdev/netdev_device.c:101).  InitResident and the
#                library-base construction it drives are V33 services, but the
#                driver has only ever been initialised by a V40 exec.
#
#   card.resource  the PCMCIA claim opens card.resource (src/netdev/
#                netdev_pcmcia.c) and asks it to walk the card's CIS tuples.
#                V37 is where card.resource was introduced -- the A600's own
#                ROM -- and the version in the machine has always been V40.
#
# Both arms are the netstack bring-up, graded by the same bringup_verdict as
# the 3.1 arms, so a difference between this table and that one is a difference
# the ROM made and nothing else.
#
# SPDX-License-Identifier: MIT

set -uo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT" || exit 2

BUILD="${AMINETXDUO_BUILD:-build/cm}"
BACKEND="${AMINETXDUO_AMIBERRY_BACKEND:-ens18}"
TIMEOUT=0
ONLY=""
LIST=0

while getopts "b:t:B:a:l" opt; do
    case "$opt" in
        b) BUILD="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        B) BACKEND="$OPTARG" ;;
        a) ONLY="$OPTARG" ;;
        l) LIST=1 ;;
        *) sed -n '3,7p' "$0" >&2; exit 2 ;;
    esac
done

# arm  model cpu    board          driver  rom
#
# The two `-v31` arms are the SAME MACHINE on a 3.1 ROM and they are what turns
# a red 2.x arm into a finding: a fault that is also there on 3.1 is not a
# Kickstart 2.x fault, and without the pair there is nothing to say which it
# was.  They cost one boot each.
# The last column is a ceiling in seconds.  The two a2065 arms carry a low one
# ON PURPOSE: they reach the end of the bring-up in about twenty-five seconds
# and then hang in teardown on BOTH ROMs, so a 300 s ceiling spends ten
# minutes a night rediscovering a fault that is fully described by second
# thirty.  The ceiling is not the finding; the hang is, and it is reported.
ARMS="
romtag-zorro       A2000  68000  ariadne2       anxnet  v204  300
romtag-zorro-v31   A2000  68000  ariadne2       anxnet  v31   300
romtag-control     A2000  68000  a2065          vendor  v204  120
romtag-control-v31 A2000  68000  a2065          vendor  v31   120
cardres-v37        A600   68020  ne2000_pcmcia  anxnet  v205  420
"

selected() { # arm
    case ",$ONLY," in
        ,,) return 0 ;;
        *",$1,"*) return 0 ;;
        *) return 1 ;;
    esac
}

if [ "$LIST" = 1 ]; then
    printf '%-19s %-6s %-6s %-14s %-7s %-5s %s\n' \
           arm model cpu board driver rom ceiling_s
    printf '%s\n' "$ARMS" | while read -r a m c n d r t; do
        [ -n "$a" ] || continue
        selected "$a" || continue
        printf '%-19s %-6s %-6s %-14s %-7s %-5s %s\n' \
               "$a" "$m" "$c" "$n" "$d" "$r" "$t"
    done
    exit 0
fi

case "$BACKEND" in
    slirp|slirp_inbound|none)
        echo "kick2x_backend=refused:$BACKEND" >&2
        echo "This is a bring-up on the wire.  -B names a host interface." >&2
        exit 2 ;;
esac

[ -f "$ROOT/$BUILD/tests/netstack/netstack_test" ] || {
    echo "kick2x_stage=missing:$BUILD/tests/netstack/netstack_test" >&2
    exit 2; }

# THE TWO 2.x ROMs.  Named rather than globbed: 2.04 is the A500+ ROM and has
# no card.resource in it at all, and 2.05 is the A600's, which is the first
# one that does.  Booting the wrong one reads as a driver that cannot find its
# card.
rom_for() { # v204|v205
    case "$1" in
    v204) printf '%s\n' "${AMINETXDUO_KICKSTART_V204:-\
${AMINETXDUO_KICKSTART_A2000:-\
$HOME/amiga-assets/roms/Kickstart v2.04 r37.175 (1991)(Commodore)(A500+)[!].rom}}" ;;
    v205) printf '%s\n' "${AMINETXDUO_KICKSTART_V205:-\
$HOME/amiga-assets/roms/Kickstart v2.05 r37.350 (1992)(Commodore)(A600HD)[!].rom}" ;;
    # The 68000 machines' 3.1, for the paired control arms.  Not the A1200
    # 40.68 image: that one does not boot an A500, A600 or A2000 at all.
    v31)  printf '%s\n' "${AMINETXDUO_KICKSTART_V31:-\
$HOME/amiga-assets/roms/Kickstart v3.1 r40.63 (1993)(Commodore)(A500-A600-A2000)[!].rom}" ;;
    esac
}

# shellcheck source=../../tools/sana2-stage.sh
. "$ROOT/tools/sana2-stage.sh"
# shellcheck source=bringup-verdict.sh
. "$ROOT/tests/tools/bringup-verdict.sh"

RESULTS="$ROOT/build/kick2x-results.txt"
: > "$RESULTS"
VERDICTS="$ROOT/build/kick2x-verdicts.txt"
: > "$VERDICTS"
LOGDIR="$ROOT/build/kick2x-logs"
rm -rf "$LOGDIR"; mkdir -p "$LOGDIR"

COUNT=0; FAILED=0; SKIPPED=0

run_arm() { # arm model cpu board driver rom ceiling
    local arm="$1" model="$2" cpu="$3" board="$4" driver="$5" rom="$6"
    local tag="kick2x-$arm" t="${TIMEOUT:-0}" rom_path rc report verdict started
    local ceiling="$7"
    local kickvar drvname="" drvsource="$driver"

    rom_path=$(rom_for "$rom")
    if [ ! -f "$rom_path" ]; then
        printf 'arm=%s status=skip reason="no %s ROM at %s"\n' \
               "$arm" "$rom" "$rom_path" | tee -a "$RESULTS"
        SKIPPED=$((SKIPPED + 1))
        return 0
    fi

    [ "$t" != 0 ] || t="$ceiling"

    # AMINETXDUO_KICKSTART_<MODEL> is what tools/amiberry-run.sh reads, and it
    # beats AMINETXDUO_KICKSTART.  Set for this arm only: the lab's env.sh
    # points A600 at a 3.1 ROM, which is the whole thing this arm is not.
    kickvar="AMINETXDUO_KICKSTART_$model"

    echo
    echo "=============================================================="
    echo "==> $arm: $model at $cpu, $board, Kickstart $rom, ${t}s"
    echo "    $(basename "$rom_path")"
    echo "=============================================================="

    started=$(date +%s)
    (
        export AMINETXDUO_RUN_TAG="$tag"
        export "$kickvar=$rom_path"

        # anxnet.device or the vendor driver, chosen the way the card sweeps
        # choose it.  The control arm sets nothing and lets
        # tests/netstack/run-amiberry.sh find Commodore's a2065.device, which
        # is what makes it a control: if our romtag is the thing 2.04 will not
        # take, that arm still comes up.
        if [ "$drvsource" = anxnet ]; then
            sana2_select "$board" "$ROOT/$BUILD"
            [ -n "$SANA2_SEL_PATH" ] || {
                echo "no anxnet.device in $BUILD" >&2; exit 2; }
            export AMINETXDUO_SANA2_DRIVER="$SANA2_SEL_PATH"
            export AMINETXDUO_SANA2_DRIVER_NAME="$SANA2_SEL_DRIVER"
            export AMINETXDUO_SANA2_DEVICE="$SANA2_SEL_DRIVER"
            export AMINETXDUO_SANA2_CARD="$SANA2_SEL_CARD"
        fi

        "$ROOT/tests/netstack/run-amiberry.sh" \
            -N "$board" -m "$model" -c "$cpu" -B "$BACKEND" -t "$t" -b "$BUILD"
    ) > "$LOGDIR/$arm.log" 2>&1
    rc=$?
    tail -30 "$LOGDIR/$arm.log"

    if [ "$drvsource" = anxnet ]; then
        drvname=anxnet.device
    else
        drvname=$(sana2_driver_for "$board")
    fi

    report="$ROOT/build/amiberry-testhd-$tag/stdout.txt"
    verdict=FAIL
    if bringup_verdict "$report" | sed 's/^/    /'; then
        verdict=PASS
    fi

    # AND THE RUN ITSELF.  bringup_verdict grades the transcript, and a
    # transcript can be complete and green on a run that then took an illegal
    # instruction outside ROM and never wrote DH0:.done.  That is exactly what
    # romtag-control does, and grading the transcript alone called it a pass.
    # rc 4 is amiberry-run.sh's illegal-instruction code, 5 the wrong backend,
    # 124 the timeout; all three are the run failing after the bring-up
    # succeeded, and all three matter here.
    if [ "$rc" != 0 ] && [ "$verdict" = PASS ]; then
        verdict=FAIL_AFTER_BRINGUP
        echo "    bringup_then=run_rc=$rc"
    fi

    printf 'arm=%s model=%s cpu=%s board=%s driver=%s rom=%s verdict=%s run_rc=%s wall_s=%s log=%s\n' \
           "$arm" "$model" "$cpu" "$board" "$drvname" "$rom" "$verdict" \
           "$rc" "$(( $(date +%s) - started ))" "$LOGDIR/$arm.log" \
           | tee -a "$RESULTS"

    COUNT=$((COUNT + 1))
    printf '%s %s\n' "$arm" "$verdict" >> "$VERDICTS"
    return 0
}

# The illegal instruction, quoted from the emulator log rather than described:
# a run that ends this way names an address, and the address is the whole of
# the diagnosis.
illegal_in() { # tag
    grep -aE "Illegal instruction: [0-9a-f]+ at [0-9A-F]+" \
         "$ROOT/build/amiberry-$1.log" 2>/dev/null |
        grep -avE "at 00F[0-9A-F]{5}" | head -2
}

while read -r arm model cpu board driver rom ceiling; do
    [ -n "$arm" ] || continue
    selected "$arm" || continue
    run_arm "$arm" "$model" "$cpu" "$board" "$driver" "$rom" "$ceiling"
done <<EOF
$ARMS
EOF

# ------------------------------------------------------- grading the PAIR --
#
# THE 3.1 PAIR IS THE CONTROL AND IT IS WHAT DECIDES.  An arm that is red on
# 2.x and green on 3.1 is a Kickstart finding and this gate's failure.  An arm
# that is red on BOTH is a fault of the machine, the driver or the tree that
# has nothing to do with the ROM, and counting it here would put a known red in
# a gate for a reason this gate cannot act on -- it is reported by name
# instead, with both verdicts beside each other so nobody has to take that on
# trust.  An arm with no `-v31` row is graded on its own.
verdict_of() { # arm
    awk -v a="$1" '$1 == a { print $2; exit }' "$VERDICTS"
}

SAME_ON_31=0
while read -r arm _m _c _b _d _r _t; do
    [ -n "$arm" ] || continue
    selected "$arm" || continue
    case "$arm" in *-v31) continue ;; esac

    _v=$(verdict_of "$arm")
    [ -n "$_v" ] || continue
    [ "$_v" = PASS ] && continue

    _p=$(verdict_of "$arm-v31")
    if [ -n "$_p" ] && [ "$_p" != PASS ]; then
        printf 'arm=%s status=same_on_31 verdict_2x=%s verdict_31=%s reason="red on Kickstart 3.1 as well, so it is not a Kickstart 2.x finding"\n' \
               "$arm" "$_v" "$_p" | tee -a "$RESULTS"
        SAME_ON_31=$((SAME_ON_31 + 1))
    else
        FAILED=$((FAILED + 1))
    fi
done <<EOF
$ARMS
EOF

echo
echo "======================== Kickstart 2.x ========================"
cat "$RESULTS"
echo "==============================================================="
echo "kick2x_arms=$COUNT kick2x_failed=$FAILED kick2x_skipped=$SKIPPED\
 kick2x_same_on_31=$SAME_ON_31"

while read -r arm _rest; do
    [ -n "$arm" ] || continue
    selected "$arm" || continue
    _ill=$(illegal_in "kick2x-$arm")
    [ -n "$_ill" ] || continue
    printf 'arm=%s illegal_instruction="%s"\n' "$arm" \
           "$(printf '%s' "$_ill" | head -1 | sed 's/.*: //')"
done <<EOF
$ARMS
EOF

[ "$COUNT" -gt 0 ] || { echo "result=refused"; echo "no arm ran" >&2; exit 2; }

if [ "$FAILED" = 0 ]; then
    echo "result=ok"
    exit 0
fi

echo "result=fail"
echo >&2
echo "Read the table by COLUMN:" >&2
echo >&2
echo "  * romtag-zorro red, romtag-control green   the ROM took Commodore's" >&2
echo "    driver and not ours, so it is our romtag or our init that V37" >&2
echo "    exec will not have.  src/netdev/netdev_device.c:101." >&2
echo >&2
echo "  * both Zorro arms red    the machine, not the driver: 2.04 on this" >&2
echo "    model, or the board never autoconfigured.  Read the emulator log." >&2
echo >&2
echo "  * cardres-v37 red alone  card.resource V37 does not answer what V40" >&2
echo "    does.  src/netdev/netdev_pcmcia.c, and the same board is green on" >&2
echo "    3.1 in tests/tools/run-cardsweep.sh." >&2
exit 1
