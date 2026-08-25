#!/usr/bin/env bash
#
# ConfigureNetInterface ON AN INTERFACE THAT HAS ONLY IPv6.
#
#   tests/tools/run-ifconfigure6.sh [-m MODEL] [-t SECONDS] [-b BUILDDIR]
#                                   [-N board] [-B backend]
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

MODEL=A1200
TIMEOUT=120
BUILD="${AMINETXDUO_BUILD:-build/cm}"
BOARD="${AMINETXDUO_AMIBERRY_BOARD:-a2065}"
IFACE="${AMINETXDUO_AMIBERRY_BACKEND:-ens18}"

while getopts "m:t:b:N:B:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        N) BOARD="$OPTARG" ;;
        B) IFACE="$OPTARG" ;;
        *) sed -n '3,6p' "$0" >&2; exit 2 ;;
    esac
done

case "$IFACE" in
    slirp|slirp_inbound|none)
        echo "ifconf6_backend=refused:$IFACE" >&2
        echo "This harness is bridged only.  -B names a host interface." >&2
        exit 2
        ;;
esac

case "$BUILD" in /*) ;; *) BUILD="${BUILD#./}" ;; esac

TOOLS="$ROOT/$BUILD/src/tools"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"

for f in "$TOOLS/ToolsSmoke" "$TOOLS/AddNetInterface" \
         "$TOOLS/ConfigureNetInterface" "$TOOLS/netstat" "$BSD"; do
    [ -f "$f" ] || { echo "ifconf6_stage=missing:$f" >&2; exit 2; }
done

A2065="${AMINETXDUO_A2065:-}"
if [ -z "$A2065" ]; then
    for candidate in \
        "$ROOT/build/a2065.device" \
        "$HOME/amiga-assets/devs/a2065.device" \
        "$HOME/amiga-os-src/os-source/other_networking/sana2/bin/devs/a2065.device"
    do
        [ -f "$candidate" ] && { A2065="$candidate"; break; }
    done
fi
[ -n "$A2065" ] && [ -f "$A2065" ] || {
    echo "ifconf6_stage=missing:a2065.device" >&2
    echo "Set AMINETXDUO_A2065=<path>." >&2
    exit 2
}

SELF6="${AMINETXDUO_IFCONF6_SELF6:-2001:db8:6726:1::10/64}"
GW6_A="${AMINETXDUO_IFCONF6_GW_A:-fe80::1}"
GW6_B="${AMINETXDUO_IFCONF6_GW_B:-fe80::2}"


STAGE="$ROOT/build/ifconfigure6-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$A2065" "$STAGE/devs/a2065.device"

cat > "$STAGE/devs/NetInterfaces/eth0" <<IFEOF
DEVICE=a2065.device
UNIT=0
CONFIGURE=NONE
CONFIGURE6=STATIC
ADDRESS6=$SELF6
STATE=up
IFEOF

. "$ROOT/tools/sana2-stage.sh"

if [ -z "${AMINETXDUO_SANA2_DRIVER:-}" ] && [ "$BOARD" != a2065 ]; then
    _want=$(sana2_driver_for "$BOARD")
    _have=$(sana2_local_driver "$_want")
    [ -n "$_have" ] && [ -f "$_have" ] &&
        export AMINETXDUO_SANA2_DRIVER="$_have"
fi

sana2_stage "$BOARD" "$STAGE/devs"

cp "$BSD"                         "$STAGE/libs/bsdsocket.library"
cp "$TOOLS/AddNetInterface"       "$STAGE/AddNetInterface"
cp "$TOOLS/ConfigureNetInterface" "$STAGE/ConfigureNetInterface"
cp "$TOOLS/netstat"               "$STAGE/netstat"

cat > "$STAGE/commands.txt" <<EOF
SYS:AddNetInterface eth0
SYS:netstat -i
SYS:netstat -r
SYS:ConfigureNetInterface eth0
SYS:ConfigureNetInterface eth0 GATEWAY6 notanaddress
SYS:ConfigureNetInterface eth0 GATEWAY6 $GW6_A
SYS:netstat -r
SYS:ConfigureNetInterface eth0 GATEWAY6 $GW6_A
SYS:netstat -r
SYS:ConfigureNetInterface eth0 GATEWAY6 $GW6_B
SYS:netstat -r
SYS:ConfigureNetInterface eth0 ADDRESS6 2001:db8:6726:1::99
SYS:ConfigureNetInterface eth0 CONFIGURE6 DHCP
SYS:ConfigureNetInterface eth0 ADDRESS 10.77.0.5 ADDRESS6 2001:db8:6726:1::99
SYS:netstat -i
SYS:netstat -r
SYS:ConfigureNetInterface eth0 GATEWAY6 NONE
SYS:netstat -r
SYS:ConfigureNetInterface eth0 GATEWAY6 NONE
EOF


export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-ifconfigure6}"

# 02:41:4d:4e:<runbyte>:7a: the fourth byte 0x4e keeps this clear of the
# derived space in tools/amiberry-run.sh (0x49), run-cardsweep6.sh's (0x4b) and
# run-srcsel.sh's (0x4c).
RUNBYTE="${AMINETXDUO_IFCONF6_RUNBYTE:-$(printf '%02x' $((RANDOM % 256)))}"
export AMINETXDUO_AMIBERRY_MAC="${AMINETXDUO_AMIBERRY_MAC:-02:41:4d:4e:$RUNBYTE:7a}"

HD="$ROOT/build/amiberry-testhd-$AMINETXDUO_RUN_TAG"

echo "ifconf6_model=$MODEL"
echo "ifconf6_board=$BOARD"
echo "ifconf6_backend=$IFACE"
echo "ifconf6_mac=$AMINETXDUO_AMIBERRY_MAC"
echo "ifconf6_self6=$SELF6"
echo "ifconf6_gw_a=$GW6_A"
echo "ifconf6_gw_b=$GW6_B"

STARTED=$(date +%s)
set +e
"$ROOT/tools/amiberry-run.sh" -N "$BOARD" -B "$IFACE" -m "$MODEL" \
    -t "$TIMEOUT" \
    "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs" \
    "$STAGE/AddNetInterface" "$STAGE/ConfigureNetInterface" "$STAGE/netstat"
RUN_RC=$?
set -e
ELAPSED=$(( $(date +%s) - STARTED ))

echo "ifconf6_run_rc=$RUN_RC"
echo "ifconf6_elapsed_s=$ELAPSED"

REPORT="$HD/tools.txt"
[ -f "$REPORT" ] || {
    echo "ifconf6_report=missing"
    echo "ifconf6=FAILED"
    exit 1
}

echo
echo "===================== what the commands printed ====================="
cat "$REPORT"
echo "====================================================================="
echo

FAILED=0
fail() { echo "$1=FAIL"; FAILED=1; }
pass() { echo "$1=ok"; }

WANTED=$(grep -c . "$STAGE/commands.txt")
RAN=$(grep -c '^===== ' "$REPORT" || true)
echo "ifconf6_commands_wanted=$WANTED"
echo "ifconf6_commands_ran=$RAN"
if [ "$RAN" -lt "$WANTED" ]; then
    STUCK=$(sed -n "$((RAN + 1))p" "$STAGE/commands.txt")
    echo "ifconf6_stuck_at=${STUCK:-<past the end of the list>}" >&2
    echo "ifconf6=INFRA" >&2
    echo "The guest ran $RAN of $WANTED commands in ${ELAPSED}s against a" \
         "${TIMEOUT}s ceiling.  That command hung; raising -t is not the fix." >&2
    exit 2
fi

block() {
    awk -v banner="$1" -v want="$2" '
        index($0, "===== " banner " =====") == 1 { n++; if (n == want) { on = 1; next } }
        on && /^----- rc / { print; exit }
        on { print }
    ' "$REPORT"
}

# The rc ToolsSmoke recorded.  RETURN_OK 0, WARN 5, ERROR 10, FAIL 20.
rc_of() { block "$1" "$2" | sed -n 's/^----- rc \([0-9-]*\),.*/\1/p'; }

want_rc() { # key banner nth expected
    local got; got=$(rc_of "$2" "$3")
    if [ "$got" = "$4" ]; then pass "$1"
    else fail "$1"; echo "  wanted rc $4, got '${got:-nothing}'" >&2
         block "$2" "$3" | sed 's/^/  /' >&2
    fi
}

says() { # key banner nth pattern
    if block "$2" "$3" | grep -Eq -- "$4"; then pass "$1"
    else fail "$1"; block "$2" "$3" | sed 's/^/  /' >&2; fi
}

denies() { # key banner nth pattern
    if block "$2" "$3" | grep -Eq -- "$4"; then
        fail "$1"; block "$2" "$3" | sed 's/^/  /' >&2
    else pass "$1"; fi
}

routes() { block "SYS:netstat -r" "$1"; }

routers6_via() { # nth-netstat next-hop
    routes "$1" | grep -E '(^|[[:space:]])::/0[[:space:]]' \
                | grep -Ec "[[:space:]]$2([[:space:]]|$)" || true
}


ADDS=$(grep -c "SYS:AddNetInterface" "$REPORT" || true)
echo "ifconf6_adds=$ADDS"
if [ "$ADDS" -eq 1 ]; then pass ifconf6_no_reset; else fail ifconf6_no_reset; fi


want_rc ifconf6_add_rc          "SYS:AddNetInterface eth0" 1 0
says    ifconf6_up_has_v6       "SYS:netstat -i" 1 '2001:db8:6726:1::10'
says    ifconf6_up_has_no_v4    "SYS:netstat -i" 1 '^eth0[[:space:]]+[0-9]+[[:space:]]+0\.0\.0\.0'

want_rc ifconf6_bare_rc         "SYS:ConfigureNetInterface eth0" 1 10
says    ifconf6_bare_names_gw6  "SYS:ConfigureNetInterface eth0" 1 'GATEWAY6'

want_rc ifconf6_badgw_rc \
    "SYS:ConfigureNetInterface eth0 GATEWAY6 notanaddress" 1 10


BEFORE_A=$(routers6_via 1 "$GW6_A")
BEFORE_B=$(routers6_via 1 "$GW6_B")
echo "ifconf6_boot_via_a=$BEFORE_A"
echo "ifconf6_boot_via_b=$BEFORE_B"
if [ "$BEFORE_A" -eq 0 ] && [ "$BEFORE_B" -eq 0 ]; then
    pass ifconf6_no_router_at_boot
else fail ifconf6_no_router_at_boot; fi

want_rc ifconf6_set_a_rc \
    "SYS:ConfigureNetInterface eth0 GATEWAY6 $GW6_A" 1 0
says    ifconf6_set_a_said \
    "SYS:ConfigureNetInterface eth0 GATEWAY6 $GW6_A" 1 "IPv6 default router is $GW6_A"
says    ifconf6_set_a_shows_addr \
    "SYS:ConfigureNetInterface eth0 GATEWAY6 $GW6_A" 1 '2001:db8:6726:1::10'

AFTER_A=$(routers6_via 2 "$GW6_A")
echo "ifconf6_routers6_after_a=$AFTER_A"
if [ "$AFTER_A" -eq 1 ]; then pass ifconf6_router_a_in_table
else fail ifconf6_router_a_in_table; fi


want_rc ifconf6_set_a_again_rc \
    "SYS:ConfigureNetInterface eth0 GATEWAY6 $GW6_A" 2 0

AGAIN=$(routers6_via 3 "$GW6_A")
echo "ifconf6_routers6_again=$AGAIN"
if [ "$AGAIN" -eq 1 ]; then pass ifconf6_idempotent
else fail ifconf6_idempotent; fi


want_rc ifconf6_set_b_rc \
    "SYS:ConfigureNetInterface eth0 GATEWAY6 $GW6_B" 1 0

AFTER_B=$(routers6_via 4 "$GW6_B")
GONE_A=$(routers6_via 4 "$GW6_A")
echo "ifconf6_routers6_after_b=$AFTER_B"
echo "ifconf6_routers6_a_left=$GONE_A"
if [ "$AFTER_B" -eq 1 ] && [ "$GONE_A" -eq 0 ]; then
    pass ifconf6_replaced_not_added
else fail ifconf6_replaced_not_added; fi


ADDR6_CMD="SYS:ConfigureNetInterface eth0 ADDRESS6 2001:db8:6726:1::99"
want_rc ifconf6_address6_rc         "$ADDR6_CMD" 1 10
says    ifconf6_address6_names_file "$ADDR6_CMD" 1 'DEVS:NetInterfaces/eth0'
says    ifconf6_address6_names_fix  "$ADDR6_CMD" 1 'AddNetInterface eth0'

CONF6_CMD="SYS:ConfigureNetInterface eth0 CONFIGURE6 DHCP"
want_rc ifconf6_configure6_rc         "$CONF6_CMD" 1 10
says    ifconf6_configure6_names_file "$CONF6_CMD" 1 'DEVS:NetInterfaces/eth0'


BOTH_CMD="SYS:ConfigureNetInterface eth0 ADDRESS 10.77.0.5 ADDRESS6 2001:db8:6726:1::99"
want_rc ifconf6_mixed_rc "$BOTH_CMD" 1 10
denies  ifconf6_mixed_no_v4_written "SYS:netstat -i" 2 '10\.77\.0\.5'
says    ifconf6_mixed_still_v4_less "SYS:netstat -i" 2 \
    '^eth0[[:space:]]+[0-9]+[[:space:]]+0\.0\.0\.0'
says    ifconf6_mixed_v6_intact     "SYS:netstat -i" 2 '2001:db8:6726:1::10'

AFTER_REFUSALS=$(routers6_via 5 "$GW6_B")
echo "ifconf6_routers6_after_refusals=$AFTER_REFUSALS"
if [ "$AFTER_REFUSALS" -eq 1 ]; then pass ifconf6_refusals_changed_nothing
else fail ifconf6_refusals_changed_nothing; fi


want_rc ifconf6_clear_rc \
    "SYS:ConfigureNetInterface eth0 GATEWAY6 NONE" 1 0
says    ifconf6_clear_said \
    "SYS:ConfigureNetInterface eth0 GATEWAY6 NONE" 1 'default router is cleared'

CLEARED_A=$(routers6_via 6 "$GW6_A")
CLEARED_B=$(routers6_via 6 "$GW6_B")
echo "ifconf6_routers6_after_clear_a=$CLEARED_A"
echo "ifconf6_routers6_after_clear_b=$CLEARED_B"
if [ "$CLEARED_A" -eq 0 ] && [ "$CLEARED_B" -eq 0 ]; then pass ifconf6_cleared
else fail ifconf6_cleared; fi

want_rc ifconf6_clear_again_rc \
    "SYS:ConfigureNetInterface eth0 GATEWAY6 NONE" 2 0

echo
if [ "$FAILED" -ne 0 ]; then
    echo "ifconf6=FAILED"
    exit 1
fi

echo "ifconf6=PASSED"
exit 0
