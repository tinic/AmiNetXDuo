#!/usr/bin/env bash
# TWO MACHINES, NO CONFIGURATION, TWO NAMES.
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

MODEL=A1200
TIMEOUT=240
BUILD="${AMINETXDUO_BUILD:-build/cm}"
BOARD="${AMINETXDUO_AMIBERRY_BOARD:-a2065}"
BACKEND="${AMINETXDUO_AMIBERRY_BACKEND:-slirp}"

while getopts "m:t:b:N:B:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        N) BOARD="$OPTARG" ;;
        B) BACKEND="$OPTARG" ;;
        *) echo "usage: $0 [-m model] [-t seconds] [-b builddir]\
 [-N board] [-B backend]" >&2; exit 2 ;;
    esac
done

TOOLS="$ROOT/$BUILD/src/tools"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"

for f in "$TOOLS/ToolsSmoke" "$TOOLS/AddNetInterface" "$TOOLS/ShowNetStatus" \
         "$TOOLS/hostname" "$TOOLS/host" "$BSD"; do
    [ -f "$f" ] || { echo "missing $f, build the tree first" >&2; exit 2; }
done

# THE NAME THIS ARM CHECKS IS THE LAST THREE BYTES OF THE STATION ADDRESS, so
# the board is a variable and not a constant: each board takes mac= from the
# emulator by a different path, and ne2000_pcmcia took none of it at all until
# the emulator was patched (tools/patches/amiberry/pcmcia-ne2000-mac.diff).  A
# guest that comes up as amiga-93e88b on this rig is wearing the HOST
# interface's address and the run proved nothing.
#
# The A2065 keeps the lookup it has always had: this arm was written against
# Commodore's driver, and swapping it would change what the default proves.
if [ "$BOARD" = a2065 ]; then
    DRIVER=a2065.device
    DRIVER_CARD=""
    DRIVER_PATH="${AMINETXDUO_A2065:-}"
    if [ -z "$DRIVER_PATH" ]; then
        for candidate in \
            "$ROOT/build/a2065.device" \
            "$HOME/amiga-os-src/os-source/other_networking/sana2/bin/devs/a2065.device"
        do
            [ -f "$candidate" ] && { DRIVER_PATH="$candidate"; break; }
        done
    fi
    [ -n "$DRIVER_PATH" ] && [ -f "$DRIVER_PATH" ] || {
        echo "No a2065.device found. Set AMINETXDUO_A2065=<path>." >&2
        exit 2
    }
else
    # shellcheck source=../../tools/sana2-stage.sh
    . "$ROOT/tools/sana2-stage.sh"
    sana2_select "$BOARD" "$ROOT/$BUILD"
    DRIVER="$SANA2_SEL_DRIVER"
    DRIVER_CARD="$SANA2_SEL_CARD"
    DRIVER_PATH="$SANA2_SEL_PATH"
    [ -n "$DRIVER_PATH" ] && [ -f "$DRIVER_PATH" ] || {
        echo "no $DRIVER for $BOARD: build $BUILD, or put the vendor driver" >&2
        echo "in the store AMINETXDUO_SANA2_STORE names." >&2
        exit 2
    }
fi
echo "hostname_mac_board=$BOARD backend=$BACKEND driver=$DRIVER\
 card=${DRIVER_CARD:-none}"

MAC_A="02:41:4d:49:00:01"; WANT_A="amiga-490001"
MAC_B="02:41:4d:ab:cd:ef"; WANT_B="amiga-abcdef"

FAILED=0
fail() { echo "FAIL: $*" >&2; FAILED=1; }

boot() {
    local tag="$1" mac="$2" want="$3"
    local stage="$ROOT/build/hostname-mac-stage-$tag"

    rm -rf "$stage"
    mkdir -p "$stage/libs"
    cp -R "$ROOT/tests/netstack/devs" "$stage/devs"
    cp "$DRIVER_PATH" "$stage/devs/$DRIVER"
    cp "$BSD"   "$stage/libs/bsdsocket.library"
    for t in AddNetInterface ShowNetStatus hostname host; do
        cp "$TOOLS/$t" "$stage/$t"
    done

    # Written out rather than appended to: the fixture names one board and
    # this arm boots whichever was asked for.
    {
        echo "DEVICE=$DRIVER"
        echo "UNIT=0"
        [ -z "$DRIVER_CARD" ] || echo "CARD=$DRIVER_CARD"
        echo "CONFIGURE=DHCP"
        echo "MDNS=YES"
    } > "$stage/devs/NetInterfaces/eth0"

    if grep -qi '^[[:space:]]*ID=' "$stage/devs/NetInterfaces/eth0" ||
       grep -qi '^[[:space:]]*hostname' "$stage/devs/Internet/name_resolution"
    then
        echo "the fixture names the machine; this test needs one that does not" >&2
        exit 2
    fi

    {
        echo "SYS:AddNetInterface eth0"
        echo "wait 3"
        echo "SYS:hostname"
        echo "SYS:ShowNetStatus"
        echo "SYS:host $want.local"
    } > "$stage/commands.txt"

    export AMINETXDUO_RUN_TAG="hostname-mac-$tag"
    export AMINETXDUO_AMIBERRY_MAC="$mac"

    echo "==> booting $MODEL, $BOARD on $BACKEND, MAC $mac" >&2
    set +e
    "$ROOT/tools/amiberry-run.sh" -N "$BOARD" -B "$BACKEND" -m "$MODEL" \
        -t "$TIMEOUT" \
        "$TOOLS/ToolsSmoke" "$stage/commands.txt" "$stage/devs" "$stage/libs" \
        "$stage/AddNetInterface" "$stage/ShowNetStatus" "$stage/hostname" \
        "$stage/host" >&2
    local rc=$?
    set -e

    local report="$ROOT/build/amiberry-testhd-$AMINETXDUO_RUN_TAG/tools.txt"
    [ -f "$report" ] || {
        echo "FAIL: guest $tag wrote no $report (run rc=$rc)" >&2
        return 1
    }
    echo "$report"
}

check() {
    local tag="$1" want="$2" report="$3"

    echo
    echo "================= guest $tag, expecting $want ================="
    cat "$report"
    echo "=============================================================="

    if grep -qx "$want" "$report"; then
        echo "guest_${tag}_hostname=$want"
    else
        echo "guest_${tag}_hostname=MISSING"
        fail "guest $tag: \`hostname\` did not print $want"
    fi

    if grep -q "not named by anything" "$report"; then
        echo "guest_${tag}_source=derived"
    else
        echo "guest_${tag}_source=OTHER"
        fail "guest $tag: something named the machine, so nothing was derived"
    fi

    if grep -q "^Host name: *$want" "$report"; then
        echo "guest_${tag}_shownetstatus=$want"
    else
        echo "guest_${tag}_shownetstatus=MISSING"
        fail "guest $tag: ShowNetStatus does not say $want"
    fi

    if grep -qi "^$want.local has address " "$report"; then
        echo "guest_${tag}_mdns=$want.local"
    else
        echo "guest_${tag}_mdns=MISSING"
        fail "guest $tag: $want.local did not resolve"
    fi

    if grep -q "^Known here as: *amiga.local" "$report"; then
        echo "guest_${tag}_claimed=amiga.local"
        fail "guest $tag: the responder still claimed amiga.local"
    else
        echo "guest_${tag}_claimed=$(sed -n 's/^Known here as: *\([^ ]*\).*/\1/p' \
                                     "$report" | head -1)"
    fi
}

REPORT_A=$(boot a "$MAC_A" "$WANT_A") || FAILED=1
REPORT_B=$(boot b "$MAC_B" "$WANT_B") || FAILED=1

[ -n "${REPORT_A:-}" ] && check a "$WANT_A" "$REPORT_A"
[ -n "${REPORT_B:-}" ] && check b "$WANT_B" "$REPORT_B"

GOT_A=$(grep -oE '^amiga-[0-9a-f]{6}$' "${REPORT_A:-/dev/null}" | head -1 || true)
GOT_B=$(grep -oE '^amiga-[0-9a-f]{6}$' "${REPORT_B:-/dev/null}" | head -1 || true)

echo
echo "got_a=${GOT_A:-NONE}"
echo "got_b=${GOT_B:-NONE}"
if [ -n "$GOT_A" ] && [ -n "$GOT_B" ] && [ "$GOT_A" != "$GOT_B" ]; then
    echo "names_differ=yes"
else
    echo "names_differ=no"
    fail "the two machines did not take two different names"
fi

echo "result=$([ "$FAILED" -eq 0 ] && echo PASS || echo FAIL)"
exit "$FAILED"
