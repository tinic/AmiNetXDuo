#!/usr/bin/env bash
#
# ConfigureNetInterface ON AN INTERFACE THAT HAS ONLY IPv6.
#
#   tests/tools/run-ifconfigure6.sh [-m MODEL] [-t SECONDS] [-b BUILDDIR]
#                                   [-N board] [-B backend]
#
# WHY THIS IS A SEPARATE HARNESS FROM run-ifconfigure.sh
#
#   That one boots an interface with an IPv4 address on it and drives the IPv4
#   keywords over it.  Every assertion in it reads a dotted quad, and the
#   configuration this one is about has none: CONFIGURE=NONE with an IPv6
#   address, which the stack brings up and which nothing in that harness can
#   describe.  Adding a second interface file to it would also change what its
#   `netstat -i` assertions are reading, and those are the spine of the arm it
#   already proves.
#
# WHAT IS BEING PROVED
#
#   1. THE MACHINE COMES UP ON IPv6 ALONE.  CONFIGURE=NONE, CONFIGURE6=STATIC
#      and an ADDRESS6, added at run time by AddNetInterface, and the interface
#      is then listed with an IPv6 address and no IPv4 one.
#
#   2. ConfigureNetInterface CAN CHANGE IT.  GATEWAY6 sets the IPv6 default
#      router, and `netstat -r` shows it: the route table before and after are
#      both read, so "it took" is a difference and not a line that was there
#      all along.
#
#   3. IT REPLACES RATHER THAN ACCUMULATES.  A second GATEWAY6 with a
#      different address leaves ONE default router, not two.  NetX Duo keeps a
#      list of them, so this is the assertion that says the command's contract
#      -- one default route -- is the one it implements.
#
#   4. ASKING FOR WHAT IS ALREADY SET CHANGES NOTHING.  The route is not
#      removed and re-added, because for the moment in between the machine
#      would have no route at all.
#
#   5. GATEWAY6 NONE CLEARS IT, and clearing it twice is not an error.
#
#   6. WHAT IT CANNOT DO IS REFUSED BY NAME.  ADDRESS6 and CONFIGURE6 are in
#      the template and are refused with rc 10 and the file to edit named.  A
#      silent no-op, or an rc 0 for a change that never happened, is the thing
#      this asserts against: both are checked for the return code AND for the
#      route table being untouched afterwards.
#
#   7. A REFUSAL IN ONE FAMILY DOES NOT HALF-APPLY THE OTHER.  `ADDRESS <v4>
#      ADDRESS6 <v6>` in one call is refused before anything is written, and
#      the interface still has no IPv4 address afterwards.
#
# ADDRESSES.  The RFC 3849 documentation prefix, and link-local next hops.
#
#   2001:db8:6726:1::10/64   ADDRESS6, the guest's own            SELF6
#   fe80::1                  the first router asked for           GW6_A
#   fe80::2                  the second, which must replace it    GW6_B
#
#   Link-local next hops on purpose: a link-local address is on-link by
#   definition, so nothing in this arm depends on a prefix being advertised or
#   on anything on the LAN answering.  No packet has to reach either of them --
#   what is asserted is the route table, which is this command's output.
#
# BRIDGED, NEVER SLIRP.  -B names the host interface to bridge onto and the
# string `slirp` is refused outright.  Nothing on the LAN has to answer, so the
# bridge carries no test traffic; it is here because a result taken over slirp
# says nothing about the stack.
#
# ONE BOOT.  ToolsSmoke reopens its transcript from the top after a reset, so a
# reboot would turn this list into a shorter one that still read as a pass.
# The count of AddNetInterface blocks is asserted for that reason.
#
# OUTPUT IS key=value AND AN EXIT CODE.  Every assertion prints one
# `name=ok` or `name=FAIL` line; nothing downstream reads prose.
#
# A TIMEOUT IS A DEFECT.  The ceiling is a boot plus twice the measured work,
# and a short transcript exits 2 -- infrastructure, not a verdict on the
# command -- naming the command it stopped at.  Raising -t is never the fix.
#
# The a2065.device driver is not ours to ship: point AMINETXDUO_A2065 at one,
# or drop a copy in build/a2065.device.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

MODEL=A1200
# The measured good case is a boot, one add and eighteen commands, none of
# which waits for anything on the network.  Same arithmetic as
# run-ifconfigure.sh: a boot of its own plus twice the work.
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

# ------------------------------------------------------------- staging ---

STAGE="$ROOT/build/ifconfigure6-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$A2065" "$STAGE/devs/a2065.device"

# CONFIGURE=NONE is the whole point: no IPv4 address is asked for and none is
# coming, so ami_config_iface_wants_ipv4() answers FALSE and this is the
# configuration the command could not touch.  CONFIGURE6=STATIC rather than
# AUTO for the reason run-ifconfigure.sh is static rather than DHCP: SLAAC is a
# second writer, and it would put a router this harness did not ask for into
# the table these assertions read.
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

# Every line is asserted below by its position in this list.  The order is
# deliberate: each step leaves the machine in the state the next one needs, and
# the two refusals are followed by a route read so "it changed nothing" is a
# transcript and not an assumption.
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

# ------------------------------------------------------------------ run ---

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-ifconfigure6}"

# A FRESH MAC EVERY RUN.  A repeated MAC is a repeated link-local, and a switch
# or router still holding the previous run's neighbour entry can answer for a
# guest whose address never finished coming up -- which is exactly the shape of
# failure this arm looks for.  Pin it with AMINETXDUO_IFCONF6_RUNBYTE when a
# run has to be repeated on the same address.
#
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

# A TIMEOUT IS A DEFECT, NOT A RESULT.  A partial transcript makes every
# assertion below read the wrong block and the first few still pass, so the
# shortfall is named before anything else runs.  Exit 2: infrastructure.
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

# The Nth block for a given command banner: its output and nothing else.
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

# How many IPv6 default routers the table shows WITH THIS NEXT HOP.  netstat -r
# prints an IPv6 default route as `::/0` and then the next hop, so this counts
# the ones this harness asked for and nothing else.
#
# BY ADDRESS AND NOT BY TOTAL, deliberately.  The link is a real one, and a
# router advertising on it puts a default router in this table that no command
# here asked for -- and does it whenever it likes, so a total taken between two
# commands is a number that can change on its own.  Every claim below is about
# fe80::1 and fe80::2, which are this harness's and cannot arrive by
# themselves.
routers6_via() { # nth-netstat next-hop
    routes "$1" | grep -E '(^|[[:space:]])::/0[[:space:]]' \
                | grep -Ec "[[:space:]]$2([[:space:]]|$)" || true
}

# ---- one boot -------------------------------------------------------------

ADDS=$(grep -c "SYS:AddNetInterface" "$REPORT" || true)
echo "ifconf6_adds=$ADDS"
if [ "$ADDS" -eq 1 ]; then pass ifconf6_no_reset; else fail ifconf6_no_reset; fi

# ---- 1. it comes up on IPv6 alone -----------------------------------------

want_rc ifconf6_add_rc          "SYS:AddNetInterface eth0" 1 0
says    ifconf6_up_has_v6       "SYS:netstat -i" 1 '2001:db8:6726:1::10'
# netstat -i prints the interface's IPv4 address in its own column, so an
# interface that asked for none reads 0.0.0.0 there.  That is the state the
# command could not touch, asserted rather than assumed.
says    ifconf6_up_has_no_v4    "SYS:netstat -i" 1 '^eth0[[:space:]]+[0-9]+[[:space:]]+0\.0\.0\.0'

# Nothing to change: the message has to name GATEWAY6, because an IPv6-only
# interface reading a list of IPv4 keywords is what the row was about.
want_rc ifconf6_bare_rc         "SYS:ConfigureNetInterface eth0" 1 10
says    ifconf6_bare_names_gw6  "SYS:ConfigureNetInterface eth0" 1 'GATEWAY6'

# A GATEWAY6 that is not an address is an argument fault, not a stack fault.
want_rc ifconf6_badgw_rc \
    "SYS:ConfigureNetInterface eth0 GATEWAY6 notanaddress" 1 10

# ---- 2. GATEWAY6 sets it, and netstat -r shows the difference -------------

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
# It reports what the interface holds, which is the half an IPv6-only
# interface never had: a line with an address on it.
says    ifconf6_set_a_shows_addr \
    "SYS:ConfigureNetInterface eth0 GATEWAY6 $GW6_A" 1 '2001:db8:6726:1::10'

AFTER_A=$(routers6_via 2 "$GW6_A")
echo "ifconf6_routers6_after_a=$AFTER_A"
if [ "$AFTER_A" -eq 1 ]; then pass ifconf6_router_a_in_table
else fail ifconf6_router_a_in_table; fi

# ---- 4. asking again changes nothing --------------------------------------

want_rc ifconf6_set_a_again_rc \
    "SYS:ConfigureNetInterface eth0 GATEWAY6 $GW6_A" 2 0

AGAIN=$(routers6_via 3 "$GW6_A")
echo "ifconf6_routers6_again=$AGAIN"
if [ "$AGAIN" -eq 1 ]; then pass ifconf6_idempotent
else fail ifconf6_idempotent; fi

# ---- 3. a different one replaces rather than accumulates -------------------

want_rc ifconf6_set_b_rc \
    "SYS:ConfigureNetInterface eth0 GATEWAY6 $GW6_B" 1 0

AFTER_B=$(routers6_via 4 "$GW6_B")
GONE_A=$(routers6_via 4 "$GW6_A")
echo "ifconf6_routers6_after_b=$AFTER_B"
echo "ifconf6_routers6_a_left=$GONE_A"
if [ "$AFTER_B" -eq 1 ] && [ "$GONE_A" -eq 0 ]; then
    pass ifconf6_replaced_not_added
else fail ifconf6_replaced_not_added; fi

# ---- 6. what it cannot do is refused by name ------------------------------

ADDR6_CMD="SYS:ConfigureNetInterface eth0 ADDRESS6 2001:db8:6726:1::99"
want_rc ifconf6_address6_rc         "$ADDR6_CMD" 1 10
says    ifconf6_address6_names_file "$ADDR6_CMD" 1 'DEVS:NetInterfaces/eth0'
says    ifconf6_address6_names_fix  "$ADDR6_CMD" 1 'AddNetInterface eth0'

CONF6_CMD="SYS:ConfigureNetInterface eth0 CONFIGURE6 DHCP"
want_rc ifconf6_configure6_rc         "$CONF6_CMD" 1 10
says    ifconf6_configure6_names_file "$CONF6_CMD" 1 'DEVS:NetInterfaces/eth0'

# ---- 7. a refusal does not half-apply the other family --------------------

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

# ---- 5. NONE clears it, twice is not an error -----------------------------

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

# rc only.  Which of the two lines it prints depends on whether a router
# advertised on this link in the seconds between the two calls, and that is not
# this harness's to decide; what is being asserted is that clearing something
# that is not there is not an error.
want_rc ifconf6_clear_again_rc \
    "SYS:ConfigureNetInterface eth0 GATEWAY6 NONE" 2 0

echo
if [ "$FAILED" -ne 0 ]; then
    echo "ifconf6=FAILED"
    exit 1
fi

echo "ifconf6=PASSED"
exit 0
