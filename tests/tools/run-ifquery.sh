#!/usr/bin/env bash
# THE REGRESSION TEST FOR THE ROADSHOW INTERFACE AND STATISTICS APIs.
# IT RUNS BRIDGED AND ONLY BRIDGED.  A DHCP server on the wire is what the
# allocation half needs, and none of the assertions below knows or cares which
# server that is.
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

MODEL=A1200
TIMEOUT=400
BUILD="${AMINETXDUO_BUILD:-build/cm}"
BOARD="${AMINETXDUO_AMIBERRY_BOARD:-a2065}"
IFACE="${AMINETXDUO_IFQUERY_IFACE:-${AMINETXDUO_AMIBERRY_BACKEND:-ens18}}"

STATE_DOWN=0

while getopts "m:t:b:N:B:D" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        N) BOARD="$OPTARG" ;;
        B) IFACE="$OPTARG" ;;
        D) STATE_DOWN=1 ;;
        *) echo "usage: $0 [-m model] [-t seconds] [-b builddir] [-N board] [-B iface] [-D]" >&2; exit 2 ;;
    esac
done

live_only() {
    [ "$STATE_DOWN" = "0" ]
}

TOOLS="$ROOT/$BUILD/src/tools"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"
PROBE="$ROOT/$BUILD/tests/tools/IfProbe"
STATPROBE="$ROOT/$BUILD/tests/tools/StatProbe"
AAMPROBE="$ROOT/$BUILD/tests/tools/AamProbe"
MONPROBE="$ROOT/$BUILD/tests/tools/MonProbe"

for f in "$TOOLS/ToolsSmoke" "$TOOLS/AddNetInterface" "$TOOLS/netstat" \
         "$PROBE" "$STATPROBE" "$AAMPROBE" "$MONPROBE" "$BSD"; do
    [ -f "$f" ] || { echo "missing $f, build the tree first" >&2; exit 2; }
done

. "$ROOT/tools/sana2-stage.sh"
DRIVER=$(sana2_driver_for "$BOARD")

DRVPATH="${AMINETXDUO_SANA2_DRIVER:-}"
if [ -z "$DRVPATH" ] && [ "$DRIVER" = a2065.device ]; then
    DRVPATH="${AMINETXDUO_A2065:-}"
fi
if [ -z "$DRVPATH" ]; then
    for candidate in "$ROOT/build/$DRIVER" "$(sana2_local_driver "$DRIVER")"; do
        [ -n "$candidate" ] && [ -f "$candidate" ] && { DRVPATH="$candidate"; break; }
    done
fi
[ -n "$DRVPATH" ] && [ -f "$DRVPATH" ] || {
    echo "No $DRIVER for board $BOARD. Put one in the driver store or set" >&2
    echo "AMINETXDUO_SANA2_DRIVER=<path>." >&2
    exit 2
}

STAGE="$ROOT/build/ifquery-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"

export AMINETXDUO_SANA2_DRIVER="$DRVPATH"
sana2_stage "$BOARD" "$STAGE/devs"
echo "==> $BOARD: $SANA2_DRIVER, opened as '$SANA2_DEVICE', from $DRVPATH"

HWADDR="${AMINETXDUO_IFQUERY_MAC:-02:41:4d:49:71:01}"
for cfg in "$STAGE"/devs/NetInterfaces/*; do
    [ -f "$cfg" ] || continue
    printf 'HARDWAREADDRESS=%s\n' "$HWADDR" >> "$cfg"
done
echo "==> staged HARDWAREADDRESS=$HWADDR"

if [ "$STATE_DOWN" = "1" ]; then
    for cfg in "$STAGE"/devs/NetInterfaces/*; do
        [ -f "$cfg" ] || continue
        printf 'STATE=down\n' >> "$cfg"
        echo "==> staged $(basename "$cfg") with STATE=down"
    done
fi
cp "$BSD"   "$STAGE/libs/bsdsocket.library"
cp "$TOOLS/AddNetInterface" "$STAGE/AddNetInterface"
cp "$TOOLS/netstat" "$STAGE/netstat"
cp "$PROBE" "$STAGE/IfProbe"
cp "$STATPROBE" "$STAGE/StatProbe"
cp "$AAMPROBE" "$STAGE/AamProbe"
cp "$MONPROBE" "$STAGE/MonProbe"

AAMARG=""
[ "$STATE_DOWN" = "1" ] && AAMARG=" DOWN"

cat > "$STAGE/commands.txt" <<EOF
SYS:IfProbe
SYS:AddNetInterface eth0
SYS:IfProbe
SYS:IfProbe DOWN
SYS:netstat -s
SYS:IfProbe OFFLINE
SYS:netstat -s
SYS:IfProbe UP
SYS:StatProbe
SYS:MonProbe
SYS:AamProbe${AAMARG}
EOF

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-ifquery}"

HD="$ROOT/build/amiberry-testhd-$AMINETXDUO_RUN_TAG"

set +e
echo "==> booting $MODEL under Amiberry, $BOARD bridged on $IFACE"
"$ROOT/tools/amiberry-run.sh" -N "$BOARD" -B "$IFACE" -m "$MODEL" \
    -t "$TIMEOUT" \
    "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs" \
    "$STAGE/AddNetInterface" "$STAGE/netstat" "$STAGE/IfProbe" \
    "$STAGE/StatProbe" "$STAGE/AamProbe" "$STAGE/MonProbe"
RUN_RC=$?
set -e

REPORT="$HD/tools.txt"
[ -f "$REPORT" ] || { echo "FAIL: the guest wrote no $REPORT (run rc=$RUN_RC)" >&2; exit 1; }

echo
echo "===================== what the commands printed ====================="
cat "$REPORT"
echo "====================================================================="
echo

FAILED=0
fail() { echo "FAIL: $*" >&2; FAILED=1; }
pass() { echo "  ok: $*"; }

ip2int() {
    local a b c d
    IFS=. read -r a b c d <<EOF
$1
EOF
    [ -n "$a" ] && [ -n "$b" ] && [ -n "$c" ] && [ -n "$d" ] || return 1
    case "$a$b$c$d" in
        *[!0-9]*) return 1 ;;
    esac
    [ "$a" -le 255 ] && [ "$b" -le 255 ] && [ "$c" -le 255 ] && [ "$d" -le 255 ] || return 1
    echo $(( (a << 24) | (b << 16) | (c << 8) | d ))
}

is_netmask() {
    local n bits=0 v
    n=$(ip2int "$1") || return 1
    v=$n
    while [ $(( v & 0x80000000 )) -ne 0 ]; do
        bits=$(( bits + 1 ))
        v=$(( (v << 1) & 0xFFFFFFFF ))
    done
    [ "$v" -eq 0 ] || return 1
    [ "$bits" -ge 8 ] && [ "$bits" -le 30 ]
}

on_subnet() {
    local a m net bcast
    a=$(ip2int "$1") || return 1
    m=$(ip2int "$2") || return 1
    net=$(( a & m ))
    bcast=$(( net | (~m & 0xFFFFFFFF) ))
    [ "$a" -ne "$net" ] && [ "$a" -ne "$bcast" ]
}

same_subnet() {
    local a b m
    a=$(ip2int "$1") || return 1
    b=$(ip2int "$2") || return 1
    m=$(ip2int "$3") || return 1
    [ $(( a & m )) -eq $(( b & m )) ]
}

field_after() {
    awk -v key="$1" -v n="$2" '
        index($0, key) == 1 { print $n; exit }' "$REPORT"
}

STARTS=$(grep -c "SYS:IfProbe =====" "$REPORT" || true)
if [ "$STARTS" -eq 2 ]; then
    pass "the machine booted once and ran both probes (no reset)"
elif [ "$STARTS" -gt 2 ]; then
    fail "THE MACHINE REBOOTED: the command list restarted"
else
    echo
    echo "INCONCLUSIVE: the guest ran $STARTS of 2 IfProbe invocations." >&2
    echo "  It stopped early. Nothing below was tested, so nothing below is" >&2
    echo "  reported. Run it again; if it repeats, the guest is the subject." >&2
    echo
    echo "ifquery: INCONCLUSIVE" >&2
    exit 2
fi

LISTED=$(grep -c "^ObtainInterfaceList: 1 interface(s)" "$REPORT" || true)
if [ "$LISTED" -eq 2 ]; then
    pass "one interface listed, both times, obtained and released twice"
else
    fail "expected two listings of one interface, got $LISTED"
fi

if grep -q "^interface 1: eth0" "$REPORT"; then
    pass "ln_Name carries the name from DEVS:NetInterfaces"
else
    fail "the node's ln_Name is not the configured interface name"
fi

if grep -q "^query eth0: rc 0 " "$REPORT"; then
    pass "QueryInterfaceTagList(eth0) returned 0"
else
    fail "QueryInterfaceTagList(eth0) did not return 0"
fi

if grep -Eq "IFQ_DeviceName +$SANA2_DEVICE\$" "$REPORT"; then
    pass "IFQ_DeviceName returned a pointer to the SANA-II device name, $SANA2_DEVICE"
else
    fail "IFQ_DeviceName did not return $SANA2_DEVICE: $(grep -m1 IFQ_DeviceName "$REPORT")"
fi

if grep -Eq "IFQ_HardwareAddressSize +48$" "$REPORT"; then
    pass "IFQ_HardwareAddressSize is 48, bits, from S2_DEVICEQUERY"
else
    fail "IFQ_HardwareAddressSize is not 48 bits"
fi

if grep -Eqi "IFQ_HardwareAddress +[0-9a-f:]+ \(7th byte a5\)" "$REPORT"; then
    pass "IFQ_HardwareAddress wrote six bytes and not one more"
else
    fail "IFQ_HardwareAddress wrote past the six bytes of an Ethernet address"
fi

LEASED=""
LEASEMASK=""
if live_only; then
    LEASED=$(awk '$1 == "IFQ_Address" { print $2; exit }' "$REPORT")
    LEASEMASK=$(awk '$1 == "IFQ_NetMask" { print $2; exit }' "$REPORT")
fi

if live_only; then
    if ! grep -Eq "IFQ_Address +$LEASED \(len 16 family 2\)" "$REPORT"; then
        fail "IFQ_Address is not a well-formed sockaddr_in: $(grep -m1 IFQ_Address "$REPORT")"
    elif [ -z "$LEASEMASK" ] || ! on_subnet "$LEASED" "$LEASEMASK"; then
        fail "IFQ_Address $LEASED is not a host address on $LEASEMASK"
    else
        pass "IFQ_Address is a sockaddr_in holding $LEASED, a host address on $LEASEMASK"
    fi
fi

if live_only; then
    if [ -n "$LEASEMASK" ] && is_netmask "$LEASEMASK"; then
        pass "IFQ_NetMask is a contiguous netmask, $LEASEMASK"
    else
        fail "IFQ_NetMask is not a netmask: ${LEASEMASK:-nothing came back}"
    fi
fi

if live_only; then
    if grep -Eq "IFQ_State +3$" "$REPORT"; then
        pass "IFQ_State is SM_Up on a live interface"
    else
        fail "IFQ_State is not SM_Up"
    fi
fi

if live_only; then
    if grep -Eq "IFQ_AddressBindType +2$" "$REPORT"; then
        pass "IFQ_AddressBindType is IFABT_Dynamic for a DHCP lease"
    else
        fail "IFQ_AddressBindType is not IFABT_Dynamic"
    fi
fi

if grep -Eq "IFQ_MTU +1500$" "$REPORT"; then
    pass "IFQ_MTU is the driver's 1500"
else
    fail "IFQ_MTU is not 1500"
fi

if grep -q "IFQ_GetBytesIn           unanswered" "$REPORT"; then
    pass "IFQ_GetBytesIn was left alone, as documented"
else
    fail "IFQ_GetBytesIn wrote something, there are no byte counters"
fi

if grep -q "IFQ_LastStart            unanswered" "$REPORT"; then
    pass "IFQ_LastStart was left alone, as documented"
else
    fail "IFQ_LastStart wrote something, nothing records it"
fi

if grep -q "^query nosuchif: .*, refused, correctly" "$REPORT"; then
    pass "an unknown interface name fails rather than returning 0"
else
    fail "querying an unknown interface did not fail"
fi

if grep -q "with no tags: rc 0, accepted, correctly" "$REPORT"; then
    pass "an empty tag list is 'does this interface exist?', and succeeds"
else
    fail "an empty tag list was refused"
fi

if grep -q "^ReleaseInterfaceList(NULL): returned" "$REPORT"; then
    pass "ReleaseInterfaceList(NULL) did nothing, as documented"
else
    fail "ReleaseInterfaceList(NULL) did not return"
fi

if grep -q "config: mask+metric: .*, refused, correctly" "$REPORT"; then
    pass "a tag list containing an unsupported tag is refused"
else
    fail "IFC_Metric was accepted, or the call did not run"
fi

if live_only; then
    if grep -q "config: mask after the refusal: .*, unchanged, correctly" "$REPORT"; then
        pass "and nothing in that list was applied: validate first, then act"
    else
        fail "the refused list changed the netmask, the call is not atomic"
    fi
fi

if grep -q "config: metric 0: .*, accepted, correctly" "$REPORT"; then
    pass "IFC_Metric 0 names what IFQ_Metric reports, and is accepted"
else
    fail "IFC_Metric 0 was refused even though IFQ_Metric answers 0"
fi

if grep -q "config: BOOL tags at FALSE: .*, accepted, correctly" "$REPORT"; then
    pass "IFC_AssociatedRoute and IFC_SetDebugMode at FALSE are accepted"
else
    fail "a BOOL configure tag set FALSE was refused"
fi

if grep -q "config: IFC_AssociatedRoute TRUE: .*, refused, correctly" "$REPORT"; then
    pass "and TRUE is still refused, nothing here tears a route down"
else
    fail "IFC_AssociatedRoute TRUE was accepted although nothing acts on it"
fi

if grep -q "config: bad address: .*, refused, correctly" "$REPORT"; then
    pass "an address string that is neither dotted-quad nor a host is refused"
else
    fail "a malformed IFC_Address was accepted"
fi

if grep -q "config: IFC_LimitMTU 576: rc 0, IFQ_MTU now 576" "$REPORT"; then
    pass "IFC_LimitMTU lowered the MTU and IFQ_MTU reports it"
else
    fail "IFC_LimitMTU did not lower the MTU"
fi

if grep -q "config: IFC_LimitMTU 9000: rc 0, IFQ_MTU now 1500" "$REPORT"; then
    pass "a request above the hardware MTU is clamped to 1500, not refused"
else
    fail "IFC_LimitMTU 9000 was not clamped to the driver's 1500"
fi

if live_only; then
    MOVED="${LEASED%.*}.200"
    if grep -Eq "config: address -> $MOVED: rc 0, IFQ_Address now $MOVED\$" "$REPORT"; then
        pass "IFC_Address moved the interface address to $MOVED"
    else
        fail "IFC_Address did not move the interface address to $MOVED: $(grep -m1 'config: address ->' "$REPORT")"
    fi
fi

if live_only; then
    if grep -Eq "config: address restored: rc 0, IFQ_Address now $LEASED\$" "$REPORT"; then
        pass "and IFC_Address with IFC_NetMask put $LEASED back in one call"
    else
        fail "the address was not restored to $LEASED: $(grep -m1 'config: address restored' "$REPORT")"
    fi
fi

if grep -q "config: SM_Down: .*, down, correctly" "$REPORT"; then
    pass "IFC_State SM_Down took the interface down"
else
    fail "IFC_State SM_Down did not take the interface down"
fi

if grep -q "config: SM_Online: .*, up, correctly" "$REPORT"; then
    pass "IFC_State SM_Online brought it back, and IFQ_State reports SM_Up"
else
    fail "IFC_State SM_Online did not bring the interface back"
fi

if grep -q "config: IFC_State 99: .*, refused, correctly" "$REPORT"; then
    pass "an IFC_State value the API never defined is refused"
else
    fail "IFC_State 99 was accepted"
fi

if grep -q "config: nosuchif: .*, refused, correctly" "$REPORT"; then
    pass "configuring an interface that does not exist is refused"
else
    fail "ConfigureInterfaceTagList accepted an unknown interface"
fi

if grep -q "^remove nosuchif: .*, refused, correctly" "$REPORT"; then
    pass "RemoveInterface refuses a name that is not there"
else
    fail "RemoveInterface accepted an unknown interface"
fi

if grep -q "^remove eth0: rc 1 .*, removed, correctly" "$REPORT"; then
    pass "RemoveInterface returned TRUE, not 0-for-success"
else
    fail "RemoveInterface did not return TRUE"
fi

if grep -q "^after remove: 0 interface(s), eth0 is gone, correctly" "$REPORT"; then
    pass "the interface left the list"
else
    fail "the interface is still listed after RemoveInterface"
fi

if grep -q "^query the removed eth0: .*, refused, correctly" "$REPORT"; then
    pass "and nothing can be queried about it any more"
else
    fail "QueryInterfaceTagList still answers for a removed interface"
fi

if grep -q "^add with an unsupported tag: .*, refused, correctly" "$REPORT"; then
    pass "AddInterfaceTagList refuses a tag that would change what is seen"
else
    fail "AddInterfaceTagList accepted IFA_PacketFilterMode PFM_Everything"
fi

if grep -q "^add eth0 ($SANA2_DEVICE unit 0): rc 0 .*, added, correctly" "$REPORT"; then
    pass "AddInterfaceTagList accepts the advisory tuning tags and adds"
else
    fail "AddInterfaceTagList refused a tuning tag and did not re-add"
fi

if grep -q "^after add: 1 interface(s), eth0 is there, correctly" "$REPORT"; then
    pass "and the refused add above left nothing half-created"
else
    fail "the interface count is wrong after the re-add"
fi

if grep -q "^add eth0 twice: .*, refused, correctly" "$REPORT"; then
    pass "a second interface of the same name is refused"
else
    fail "two interfaces were allowed to share a name"
fi

if grep -qi "^  IFQ_HardwareAddress *$HWADDR " "$REPORT"; then
    pass "HARDWAREADDRESS=$HWADDR was committed to the card"
else
    GOT=$(grep -m1 "^  IFQ_HardwareAddress" "$REPORT" | awk '{print $2}')
    fail "HARDWAREADDRESS=$HWADDR did not reach the card: it reports ${GOT:-nothing}." \
         "Either the keyword is being dropped again, or this driver ignores" \
         "the address S2_CONFIGINTERFACE commits."
fi

if grep -q "^hardware address after the round trip: .*, the device was reopened, correctly" "$REPORT"; then
    pass "the SANA-II device was really closed and really reopened"
else
    fail "the re-added interface did not report the card's own address"
fi

if grep -q "^state after: 3, .*, up again, correctly" "$REPORT"; then
    pass "and it configures and comes up like any other interface"
else
    fail "the re-added interface would not come up"
fi

for case in "version 0" "type 99" "NETSTATUS_mb"; do
    if grep -q "^$case: .*, refused, correctly" "$REPORT"; then
        pass "GetNetworkStatistics refused: $case"
    else
        fail "GetNetworkStatistics accepted: $case"
    fi
done

if grep -q "^NETSTATUS_ip size: .*, sizeof(struct ipstat), correctly" "$REPORT"; then
    pass "a NULL destination returns the size a complete copy would need"
else
    fail "GetNetworkStatistics(NULL) did not return sizeof(struct ipstat)"
fi

if live_only; then
    if grep -Eq "^NETSTATUS_ip: rc 96 total [1-9][0-9]* localout [1-9][0-9]* " "$REPORT"; then
        pass "ipstat carries the running stack's packet counts"
    else
        fail "ipstat came back zeroed or the wrong size"
    fi
fi

if live_only; then
    if grep -Eq "^NETSTATUS_udp: rc 36 ipackets [1-9][0-9]* opackets [1-9][0-9]* " "$REPORT"; then
        pass "udpstat carries the DHCP exchange this machine actually had"
    else
        fail "udpstat came back zeroed or the wrong size"
    fi
fi

GUARDS=$(grep -c "guard intact" "$REPORT" || true)
OVERRUNS=$(grep -c "OVERRUN" "$REPORT" || true)
if [ "$OVERRUNS" -eq 0 ] && [ "$GUARDS" -ge 6 ]; then
    pass "no copy wrote past its bound ($GUARDS guards checked)"
else
    fail "$OVERRUNS copy/copies overran the caller's buffer"
fi

if grep -q "^NETSTATUS_ip into 8 bytes: rc 8 " "$REPORT"; then
    pass "a caller asking for 8 bytes gets 8 and is told so"
else
    fail "a partial request did not return the partial count"
fi

if grep -q "^tcp sockets after listen: .*, one more connection, correctly" "$REPORT"; then
    pass "NETSTATUS_tcp_sockets grew by exactly one entry after listen()"
else
    fail "the socket table did not grow by one entry"
fi

if grep -q "^listener state: 1, TCPS_LISTEN, correctly" "$REPORT"; then
    pass "pcd_tcp_state is 4.4BSD's enumeration, not NetX Duo's"
else
    fail "pcd_tcp_state did not come back as TCPS_LISTEN"
fi

if grep -q "^tcp table into one entry: .*, one entry, correctly" "$REPORT"; then
    pass "a buffer that holds one entry gets one entry and no more"
else
    fail "the socket table ignored the caller's size limit"
fi

for case in "no result ptr:CAAME_Invalid_result_ptr" \
            "version 99:CAAME_Invalid_version" \
            "protocol 77:CAAME_Invalid_protocol" \
            "an empty name:CAAME_Invalid_interface_name" \
            "an unknown interface:CAAME_Interface_not_found" \
            "a 1-character client id:CAAME_Client_identifier_too_short" \
            "a 299-character client id:CAAME_Client_identifier_too_long"; do
    what=${case%%:*}
    code=${case##*:}
    if grep -q "^create \(with \)\?\(for \)\?$what: .*, correctly" "$REPORT"; then
        pass "CreateAddrAllocMessageA returns $code"
    else
        fail "CreateAddrAllocMessageA got $what wrong"
    fi
done

if grep -q "^create with every buffer: 0, message allocated" "$REPORT"; then
    pass "CreateAddrAllocMessageA built a message with every buffer asked for"
else
    fail "CreateAddrAllocMessageA could not build a full message"
fi

if grep -q "^timeout asked 3, got 10, extended, correctly" "$REPORT"; then
    pass "a timeout below the documented minimum is extended, not refused"
else
    fail "the 10-second minimum timeout was not applied"
fi

if grep -q "^reply port set, mn_Length .*, correctly" "$REPORT"; then
    pass "the message is a well-formed struct Message, mn_Length and all"
else
    fail "the message was not initialised for ReplyMsg()"
fi

if grep -q "^client id .*, duplicated, correctly" "$REPORT"; then
    pass "the client identifier was duplicated into the message"
else
    fail "the client identifier was not duplicated"
fi

if grep -q "^buffers: all present, aligned and distinct, correctly" "$REPORT"; then
    pass "every buffer is present, longword-aligned and distinct"
else
    fail "the carved buffers overlap, are misaligned or are missing"
fi

if grep -q "^buffers zeroed: yes, correctly" "$REPORT"; then
    pass "and zeroed, so a caller reading them after the reply sees no rubbish"
else
    fail "the carved buffers were not zeroed"
fi

if [ "$STATE_DOWN" = "1" ]; then
    if grep -q "^staged: down, the lease half is skipped" "$REPORT"; then
        pass "AamProbe was told the interface was staged down and skipped the lease half"
    else
        fail "AamProbe did not get its STATE argument, so it asked for a lease on a down interface"
    fi
fi

if grep -q "^unicast 1, honoured at version 2, correctly" "$REPORT"; then
    pass "CAAMTA_RequestUnicast is honoured at AAM_VERSION 2"
else
    fail "CAAMTA_RequestUnicast was not stored"
fi

begin_replied() {
    if grep -q "^begin $1: .* replied, correctly" "$REPORT"; then
        pass "BeginInterfaceConfig replied the message: $1"
    else
        fail "BeginInterfaceConfig did not reply the message: $1"
    fi
}

if live_only; then
    begin_replied "on an addressed interface"
fi
begin_replied "on an unknown interface"
begin_replied "with a bad version"

if grep -q "^AbortInterfaceConfig(NULL): returned" "$REPORT"; then
    pass "AbortInterfaceConfig is safe with nothing in flight and with NULL"
else
    fail "AbortInterfaceConfig did not return"
fi

if live_only; then
    if grep -q "^live: begin returned with the message still out, asynchronous, correctly" "$REPORT"; then
        pass "BeginInterfaceConfig returned before the allocation finished"
    else
        fail "BeginInterfaceConfig blocked its caller, it is documented asynchronous"
    fi

    if grep -q "^live: replied after .* result 0, AAMR_Success, correctly" "$REPORT"; then
        pass "and the message came back with AAMR_Success"
    else
        fail "the allocation did not succeed"
    fi
fi

LIVE_ADDR=$(field_after "live: address" 3)
LIVE_MASK=$(field_after "live: address" 5)
LIVE_SERVER=$(field_after "live: address" 7)

if ! live_only; then
    : "no lease was asked for, so there is none to read"
elif [ -z "$LIVE_ADDR" ]; then
    fail "the allocation printed no address line at all"
elif ! is_netmask "${LIVE_MASK:-}"; then
    fail "the lease mask is not a netmask: ${LIVE_MASK:-nothing}"
elif ! on_subnet "$LIVE_ADDR" "$LIVE_MASK"; then
    fail "the leased $LIVE_ADDR is not a host address on $LIVE_MASK"
elif live_only && [ -n "$LEASEMASK" ] && [ "$LIVE_MASK" != "$LEASEMASK" ]; then
    fail "the same server gave this interface $LEASEMASK at boot and $LIVE_MASK now"
elif [ "${LIVE_SERVER:-0.0.0.0}" = "0.0.0.0" ]; then
    fail "the lease carries no server address, DHCP option 54 was not stored"
elif ! same_subnet "$LIVE_SERVER" "$LIVE_ADDR" "$LIVE_MASK"; then
    fail "the server $LIVE_SERVER is not on the subnet it leased, $LIVE_ADDR/$LIVE_MASK"
else
    pass "the lease carries the server's own numbers: $LIVE_ADDR mask $LIVE_MASK from $LIVE_SERVER"
fi

LIVE_ROUTER=$(field_after "live: router[0]" 3)
LIVE_ROUTER="${LIVE_ROUTER%,}"

if ! live_only; then
    : "no lease, no router table"
elif [ -z "$LIVE_ROUTER" ] || grep -q "^live: router\[0\] .*, none offered" "$REPORT"; then
    fail "no router came back, the parameter request list is not being sent"
elif [ "$LIVE_ROUTER" = "0.0.0.0" ]; then
    fail "aam_RouterTable[0] is zero, option 3 was parsed into nothing"
elif ! same_subnet "$LIVE_ROUTER" "$LIVE_ADDR" "$LIVE_MASK"; then
    fail "the offered router $LIVE_ROUTER is not on the leased subnet $LIVE_ADDR/$LIVE_MASK"
else
    pass "aam_RouterTable carries $LIVE_ROUTER, a router on the leased subnet"
fi

if live_only; then
    if grep -Eq "^live: lease expires day [1-9][0-9]+ " "$REPORT"; then
        pass "aam_LeaseExpires holds a real date rather than the zero that means infinite"
    else
        fail "the lease expiry DateStamp was not filled in"
    fi

    if grep -q "^begin a second time: result 4, replied, correctly" "$REPORT"; then
        pass "a second allocation on the now-addressed interface is AAMR_AddressKnown"
    else
        fail "the allocation did not put the address on the interface"
    fi
fi

if grep -q "^slow: still running after a second: yes, correctly" "$REPORT"; then
    pass "an allocation with no server to answer is still running a second in"
else
    fail "the allocation did not stay in flight"
fi

if grep -q "^busy: .*, refused at the door with AAMR_Busy, correctly" "$REPORT"; then
    pass "a second process asking for an interface already being configured is refused AAMR_Busy"
elif grep -q "^busy: .*, NOT TESTED" "$REPORT"; then
    fail "the second process never got as far as asking, AAMR_Busy is UNTESTED"
else
    fail "the second request got a worker of its own, bsd_aam_jobs[] did not refuse it at the door, and the first caller's job is no longer the one in the table"
fi

if grep -q "^slow: abort replied after .*, AAMR_Aborted, correctly" "$REPORT"; then
    pass "AbortInterfaceConfig stopped one in flight, replied AAMR_Aborted"
else
    fail "AbortInterfaceConfig did not abort an allocation in flight"
fi

if grep -q "^slow: timeout replied after .*, AAMR_Timeout, correctly" "$REPORT"; then
    pass "and one left alone gives up with AAMR_Timeout"
else
    fail "the allocation never timed out"
fi

if grep -q "^slow: waited at least, correctly the 10-second floor" "$REPORT"; then
    pass "it waited at least the documented ten-second minimum"
else
    fail "the allocation gave up before the ten-second floor"
fi

if grep -q "^DeleteAddrAllocMessage on a stack message: .*, refused, correctly" "$REPORT"; then
    pass "DeleteAddrAllocMessage refuses a message it did not allocate"
else
    fail "DeleteAddrAllocMessage tried to free a message it did not allocate"
fi

if grep -Eq "^state DOWN: rc 0 .* IFQ_State now 2$" "$REPORT"; then
    pass "SM_Down stopped the stack transmitting, IFQ_State reports SM_Down"
else
    fail "IFC_State SM_Down did not report SM_Down"
fi

NETSTAT_STATES=$( { grep -Eo "^eth0 \((online|offline)\)$" "$REPORT" || true; } | tr '\n' ' ')
if [ "$NETSTAT_STATES" = "eth0 (online) eth0 (offline) " ]; then
    pass "SM_Down left the SANA-II device on the network, SM_Offline took it off"
else
    fail "the device state after SM_Down/SM_Offline was: $NETSTAT_STATES"
fi

if grep -Eq "^state UP: rc 0 .* IFQ_State now 3$" "$REPORT"; then
    pass "and SM_Up brought it back"
else
    fail "SM_Up did not bring the interface back after SM_Offline"
fi

if grep -q "^SBTC_HAVE_MONITORING_API: .*, TRUE, correctly" "$REPORT"; then
    pass "SBTC_HAVE_MONITORING_API answers TRUE for the three types that work"
else
    fail "SBTC_HAVE_MONITORING_API answers FALSE although hooks install"
fi

if grep -q "^set IP_DEFAULT_TTL to its own value.*, accepted and the next tag was serviced, correctly" "$REPORT"; then
    pass "writing a tunable back at its current value is not a change"
else
    fail "a no-op SET was refused and discarded the rest of the tag list"
fi

if grep -q "^turn IP forwarding on: .*, refused at tag 1, correctly" "$REPORT"; then
    pass "and a real change to something unimplemented is still refused"
else
    fail "SBTC_IP_FORWARDING was set although this stack does not forward"
fi

for case in "a NULL hook:EFAULT" "type 99:EINVAL"; do
    what=${case%%:*}
    code=${case##*:}
    if grep -q "^add $what: .*, $code, correctly" "$REPORT"; then
        pass "AddNetMonitorHookTagList returns $code for $what"
    else
        fail "AddNetMonitorHookTagList got $what wrong"
    fi
done

if grep -q "^add MHT_Packet: .*, refused rather than silently ignored, correctly" "$REPORT"; then
    pass "an in-stack type nothing dispatches is refused, not silently ignored"
else
    fail "MHT_Packet was accepted although nothing dispatches it"
fi

if grep -q "^add the same hook twice: .*, refused, correctly" "$REPORT"; then
    pass "one Hook cannot be installed twice, removal takes no type"
else
    fail "the same Hook was accepted into two lists"
fi

if grep -q "^bind with an allowing hook: .*, allowed and seen, correctly" "$REPORT"; then
    pass "a hook that returns 0 sees the call and lets it through"
else
    fail "an allowing hook was not consulted, or blocked the call"
fi

if grep -q "^reserved was NULL, correctly, hook was ours, correctly" "$REPORT"; then
    pass "the hook was entered with A0=Hook, A2=NULL, A1=message"
else
    fail "the hook register convention is wrong"
fi

if grep -q "^message is the published shape: yes, correctly" "$REPORT"; then
    pass "bmm_Size, bmm_Socket and bmm_Name are what bind() was given"
else
    fail "the monitor message does not match what was passed to bind()"
fi

if grep -q "^bind with a denying hook: .*, denied with the hook.s errno, correctly" "$REPORT"; then
    pass "a hook that returns an errno fails bind() with exactly that errno"
else
    fail "a denying hook did not fail bind() with its own errno"
fi

if grep -q "^two hooks on one type: .*, both installed, correctly" "$REPORT"; then
    pass "more than one hook can be installed for one task"
else
    fail "a second hook on one type was refused"
fi

if grep -q "^first allows, second denies: .*, one hook cannot overrule another, correctly" "$REPORT"; then
    pass "a hook that allows cannot overrule one that denies"
else
    fail "an allowing hook overruled a denying one"
fi

if grep -q "^first denies: .*, the walk stopped, correctly" "$REPORT"; then
    pass "and the walk stops at the first refusal"
else
    fail "hooks after the first refusal were still consulted"
fi

if grep -q "^connect with a denying hook: .*, denied before the connect, correctly" "$REPORT"; then
    pass "MHT_Connect denies connect() before the stack has done anything"
else
    fail "MHT_Connect did not deny the connect"
fi

if grep -q "^send denied: .*, denied before the send, correctly" "$REPORT"; then
    pass "MHT_Send fails send() with the hook's errno, before anything is sent"
else
    fail "MHT_Send did not deny the send"
fi

if grep -q "^send shape: .*, correctly" "$REPORT"; then
    pass "send(): smm_Buffer and smm_Len are the caller's, To and Msg both NULL"
else
    fail "send() built the wrong SendMonitorMessage"
fi

if grep -q "^sendto shape: to ours msg NULL, correctly" "$REPORT"; then
    pass "sendto(): smm_To is the caller's address, smm_Msg NULL"
else
    fail "sendto() built the wrong SendMonitorMessage"
fi

if grep -q "^sendmsg shape: to NULL msg ours .*, correctly" "$REPORT"; then
    pass "sendmsg(): smm_Msg is the caller's msghdr, smm_To NULL, smm_Len total"
else
    fail "sendmsg() built the wrong SendMonitorMessage"
fi

if grep -q "^never both set: yes, correctly" "$REPORT"; then
    pass "smm_To and smm_Msg are never both set, across all three calls"
else
    fail "smm_To and smm_Msg were both set"
fi

if grep -q "^send allowed: .*, sent in full, correctly" "$REPORT"; then
    pass "and a send the hook allows goes through untouched"
else
    fail "an allowed send did not complete"
fi

if grep -q "^after removal: .*, no longer consulted, correctly" "$REPORT"; then
    pass "a removed hook stops being consulted"
else
    fail "a removed hook was still called"
fi

if grep -q "^RemoveNetMonitorHook(NULL) and twice: returned" "$REPORT"; then
    pass "RemoveNetMonitorHook is safe with NULL and with a hook already out"
else
    fail "RemoveNetMonitorHook did not survive NULL or a double removal"
fi

if [ "$STATE_DOWN" = "1" ]; then
    if grep -q "^  IFQ_State *2$" "$REPORT"; then
        pass "STATE=down was honoured: the interface came up configured down"
    else
        fail "STATE=down was ignored: $(grep -m1 '^  IFQ_State' "$REPORT")"
    fi
else
    if grep -q "^  IFQ_State *3$" "$REPORT"; then
        pass "the default config leaves the interface up"
    else
        fail "the default config did not leave the interface up"
    fi
fi

echo
if [ "$FAILED" -ne 0 ]; then
    echo "ifquery: FAILED" >&2
    exit 1
fi

echo "ifquery: PASSED"
exit 0
