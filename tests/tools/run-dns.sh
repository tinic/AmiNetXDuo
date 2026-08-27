#!/usr/bin/env bash
# THE REGRESSION TEST FOR THE RESOLVER API.
# THE ONE EXTERNAL DEPENDENCY, stated rather than hidden: the name server named
# in AMINETXDUO_DNS_STATIC has to answer for www.example.com.  The default is
# the LAN's own router.  If the fully qualified lookup fails the script says so
# BRIDGED, NEVER SLIRP.  -B names the host NIC to bridge onto and the string
# `slirp` is refused outright.  The whole file used to run behind NAT -- the
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

MODEL=A1200
TIMEOUT=600
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
        *) echo "usage: $0 [-m model] [-t seconds] [-b builddir] [-N board] [-B backend]" >&2; exit 2 ;;
    esac
done

case "$IFACE" in
    slirp|slirp_inbound|none)
        echo "dns_backend=refused:$IFACE" >&2
        echo "This harness is bridged only.  -B names a host interface." >&2
        exit 2
        ;;
esac

TOOLS="$ROOT/$BUILD/src/tools"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"
PROBE="$ROOT/$BUILD/tests/tools/DnsProbe"

for f in "$TOOLS/ToolsSmoke" "$TOOLS/AddNetInterface" "$PROBE" "$BSD"; do
    [ -f "$f" ] || { echo "missing $f, build the tree first" >&2; exit 2; }
done

A2065="${AMINETXDUO_A2065:-}"
if [ -z "$A2065" ]; then
    for candidate in \
        "$ROOT/build/a2065.device" \
        "$HOME/amiga-os-src/os-source/other_networking/sana2/bin/devs/a2065.device"
    do
        [ -f "$candidate" ] && { A2065="$candidate"; break; }
    done
fi
[ -n "$A2065" ] && [ -f "$A2065" ] || {
    echo "No a2065.device found. Set AMINETXDUO_A2065=<path>." >&2
    exit 2
}

STATIC_DNS="${AMINETXDUO_DNS_STATIC:-192.168.1.1}"
HOST="${AMINETXDUO_DNS_HOST:-www}"
DOMAIN="${AMINETXDUO_DNS_DOMAIN:-example.com}"

SELF="${AMINETXDUO_DNS_SELF:-192.168.1.239}"
GATEWAY="${AMINETXDUO_DNS_GATEWAY:-192.168.1.1}"
NETMASK="${AMINETXDUO_DNS_NETMASK:-255.255.255.0}"
# RFC 3849 documentation space: it answers nothing, which is why the nesting
# is measured on it and not on a resolver the machine is using.  Kept in step
# with PROBE_EXTRA_DNS6 in tests/tools/dnsprobe.c.
PROBE_DNS6=2001:db8::53
SELF_NAME=amiga-probe.localdomain
SELF_ALIAS=amiga-probe

export AMINETXDUO_AMIBERRY_MAC="${AMINETXDUO_DNS_MAC:-02:41:4d:49:00:d4}"

STAGE="$ROOT/build/dns-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$A2065" "$STAGE/devs/a2065.device"
cp "$BSD"   "$STAGE/libs/bsdsocket.library"
cp "$TOOLS/AddNetInterface" "$STAGE/AddNetInterface"
cp "$PROBE" "$STAGE/DnsProbe"

printf '%s %s %s\n' "$SELF" "$SELF_NAME" "$SELF_ALIAS" >> "$STAGE/devs/Internet/hosts"

cat > "$STAGE/devs/NetInterfaces/eth0" <<IFEOF
DEVICE=a2065.device
UNIT=0
CONFIGURE=STATIC
ADDRESS=$SELF
NETMASK=$NETMASK
GATEWAY=$GATEWAY
STATE=up
IFEOF

cat > "$STAGE/devs/Internet/name_resolution" <<NREOF
nameserver $STATIC_DNS
domain localdomain
NREOF

. "$ROOT/tools/sana2-stage.sh"
if [ -z "${AMINETXDUO_SANA2_DRIVER:-}" ] && [ "$BOARD" != a2065 ]; then
    _want=$(sana2_driver_for "$BOARD")
    _have=$(sana2_local_driver "$_want")
    [ -n "$_have" ] && [ -f "$_have" ] &&
        export AMINETXDUO_SANA2_DRIVER="$_have"
fi
sana2_stage "$BOARD" "$STAGE/devs"

cat > "$STAGE/commands.txt" <<EOF
SYS:AddNetInterface eth0
SYS:DnsProbe $STATIC_DNS $HOST $DOMAIN $SELF_NAME $SELF_ALIAS
EOF

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-dns}"

set +e
HD="$ROOT/build/amiberry-testhd-$AMINETXDUO_RUN_TAG"
echo "==> booting $MODEL under Amiberry, $BOARD on $IFACE"
"$ROOT/tools/amiberry-run.sh" -N "$BOARD" -B "$IFACE" -m "$MODEL" \
    -t "$TIMEOUT" \
    "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs" \
    "$STAGE/AddNetInterface" "$STAGE/DnsProbe"
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

STARTS=$(grep -c "SYS:DnsProbe" "$REPORT" || true)
if [ "$STARTS" -eq 1 ]; then
    pass "the machine booted once and ran the probe (no reset)"
else
    fail "the probe line appears $STARTS times, the machine reset"
fi

pton_rc() {
    sed -n "s/^pton \"$1\" rc \([-0-9]*\) .*/\1/p" "$REPORT" | head -1
}
addr_of() {
    sed -n "s/^pton \"$1\" rc .* | addr -> \(.*\)$/\1/p" "$REPORT" | head -1
}

for good in "1.2.3.4" "0.0.0.0" "255.255.255.255" "127.0.0.1"; do
    if [ "$(pton_rc "$good")" = "1" ]; then
        pass "inet_pton accepts $good"
    else
        fail "inet_pton rejected $good (rc $(pton_rc "$good"))"
    fi
done

for bad in "0177.0.0.1" "0x1.2.3.4" "0x7f000001" "010.1.1.1" "127.1"; do
    RC=$(pton_rc "$bad")
    LAX=$(addr_of "$bad")
    if [ "$RC" = "0" ]; then
        pass "inet_pton refuses $bad"
    else
        fail "inet_pton accepted $bad (rc $RC), octal/hex/short form"
    fi
    if [ -n "$LAX" ] && [ "$LAX" != "INADDR_NONE" ]; then
        pass "inet_addr still reads $bad as $LAX"
    else
        fail "inet_addr no longer accepts $bad, its 4.3BSD behaviour regressed"
    fi
done

for bad in "1.2.3.04" "1.2.3" "1.2.3.4.5" "256.1.1.1" "1.2.3.4 " " 1.2.3.4" "1.2.3." ""; do
    if [ "$(pton_rc "$bad")" = "0" ]; then
        pass "inet_pton refuses \"$bad\""
    else
        fail "inet_pton accepted \"$bad\""
    fi
done

if grep -q "^pton family 99 rc -1$" "$REPORT"; then
    pass "inet_pton(bad family) is -1, distinct from a malformed address"
else
    fail "inet_pton(bad family) did not answer -1"
fi

HOSTNAME_GOT=$(sed -n 's/^hostname full: rc 0 "\(.*\)"$/\1/p' "$REPORT" | head -1)
if [ -n "$HOSTNAME_GOT" ]; then
    pass "gethostname answers \"$HOSTNAME_GOT\", the machine's configured name"
else
    fail "gethostname did not answer a name at all"
    grep "^hostname full:" "$REPORT" >&2 || true
    HOSTNAME_GOT="__no_name__"
fi

case "$HOSTNAME_GOT" in
    __no_name__) ;;
    *.*) fail "gethostname answered the qualified \"$HOSTNAME_GOT\"; the bare
       label is what option 12 announces and what the mDNS label is cut from" ;;
    *)   pass "and it is the bare label, not a qualified name" ;;
esac

if grep -q "^hostname null: rc -1 errno 14$" "$REPORT"; then
    pass "gethostname(NULL) is -1 with EFAULT"
else
    fail "gethostname(NULL) did not answer -1/EFAULT"
    grep "^hostname null:" "$REPORT" >&2 || true
fi

for n in 1 2 3 4 5 6 7 8; do
    WANT=$(printf '%s' "$HOSTNAME_GOT" | cut -c "1-$n")
    if grep -Eq "^hostname $n: rc 0 errno [-0-9]+ \"$WANT\" term no\$" "$REPORT"; then
        pass "gethostname into $n bytes gives \"$WANT\", unterminated, rc 0"
    else
        fail "gethostname into $n bytes did not truncate to \"$WANT\""
        grep "^hostname $n:" "$REPORT" >&2 || true
    fi
done

NLEN=${#HOSTNAME_GOT}
if grep -Eq "^hostname $NLEN: rc 0 errno [-0-9]+ \"$HOSTNAME_GOT\" term no\$" "$REPORT"; then
    pass "the whole name in exactly its own length, and no terminator"
else
    fail "gethostname into $NLEN bytes did not give the bare name"
    grep "^hostname $NLEN:" "$REPORT" >&2 || true
fi

if grep -Eq "^hostname $((NLEN + 1)): rc 0 errno [-0-9]+ \"$HOSTNAME_GOT\" term yes\$" "$REPORT"; then
    pass "one byte more and the name is terminated"
else
    fail "gethostname into $((NLEN + 1)) bytes did not terminate the name"
    grep "^hostname $((NLEN + 1)):" "$REPORT" >&2 || true
fi

for asked in "$SELF_ALIAS" "$SELF_NAME"; do
    if grep -q "^official \"$asked\": name \"$SELF_NAME\" alias \"$SELF_ALIAS\"$" "$REPORT"; then
        pass "gethostbyname(\"$asked\") answers with the official name and the alias"
    else
        fail "gethostbyname(\"$asked\") did not answer \"$SELF_NAME\"/\"$SELF_ALIAS\""
        grep "^official \"$asked\":" "$REPORT" >&2 || true
    fi
done

if grep -q "^broadcast \"255.255.255.255\": 255.255.255.255$" "$REPORT"; then
    pass "255.255.255.255 is read as a literal, not sent to a name server"
else
    fail "gethostbyname(\"255.255.255.255\") did not answer the broadcast address"
    grep "^broadcast " "$REPORT" >&2 || true
fi

use_after() {
    sed -n "s/^$1 $2 rc [-0-9]* use \([-0-9]*\)$/\1/p" "$REPORT" | head -1
}

STATIC0=$(sed -n "s/^static $STATIC_DNS use \([-0-9]*\)$/\1/p" "$REPORT" | head -1)
if [ "$STATIC0" = "-1" ]; then
    pass "$STATIC_DNS came from the file and reports use -1"
else
    fail "$STATIC_DNS reports use '$STATIC0', expected -1 (statically configured)"
fi

if [ "$(use_after add "$STATIC_DNS")" = "-2" ]; then
    pass "AddDomainNameServer on the file's entry deepens it to -2, still static"
else
    fail "adding the file's entry gave use '$(use_after add "$STATIC_DNS")', expected -2"
fi

if [ "$(use_after remove "$STATIC_DNS")" = "-1" ]; then
    pass "one Remove undoes one Add, $STATIC_DNS is STILL in the list"
else
    fail "Remove dropped $STATIC_DNS after a single Add (use '$(use_after remove "$STATIC_DNS")')"
    echo "       this is the bug: two programs sharing a name server, the" >&2
    echo "       first to exit kills the second one's resolver." >&2
fi

EXTRA=192.0.2.53
ADDS=$(sed -n "s/^add $EXTRA rc [-0-9]* use \([-0-9]*\)$/\1/p" "$REPORT")
REMOVES=$(sed -n "s/^remove $EXTRA rc [-0-9]* use \([-0-9]*\)$/\1/p" "$REPORT")

if [ "$(echo "$ADDS" | tr '\n' ' ')" = "1 2 " ]; then
    pass "$EXTRA was added by the API: use counts up 1, 2, and POSITIVE"
else
    fail "adding $EXTRA twice gave use [$(echo "$ADDS" | tr '\n' ' ')], expected 1 2"
fi

if [ "$(echo "$REMOVES" | tr '\n' ' ')" = "1 0 " ]; then
    pass "$EXTRA needed two Removes, and left the list on the second"
else
    fail "removing $EXTRA twice gave use [$(echo "$REMOVES" | tr '\n' ' ')], expected 1 0"
fi

if grep -q "^remove $EXTRA again rc -1 errno 2$" "$REPORT"; then
    pass "one Remove too many is -1 with ENOENT, not a silent success"
else
    fail "the extra Remove did not answer -1/ENOENT"
    grep "^remove $EXTRA again" "$REPORT" >&2 || true
fi

if grep -q "^dnslist final: $STATIC_DNS use -1 " "$REPORT"; then
    pass "the list ends as it began: $STATIC_DNS, static, use -1"
else
    fail "the probe did not leave the name server list as it found it"
fi

# ---- the IPv6 half of the same interface ---------------------------------
#
# THE ROW THIS COVERS: ObtainDomainNameServerList() reported the IPv6 servers
# and neither Add nor Remove would take one, so an address the list handed a
# program back could not be given to RemoveDomainNameServer().  Everything the
# probe does after the first Add uses the text the LIST printed.

EXTRA6=$(sed -n "s/^listed6 \([0-9A-Fa-f:]*\) use [-0-9]*$/\1/p" "$REPORT" |
         head -1)
if [ -n "$EXTRA6" ]; then
    pass "ObtainDomainNameServerList reports the IPv6 server as $EXTRA6"
else
    fail "no IPv6 server reached the list at all"
    grep -E "^(add6|listed6)" "$REPORT" >&2 || true
fi

if grep -q "^add6 $PROBE_DNS6 rc 0 use 1$" "$REPORT"; then
    pass "AddDomainNameServer accepts an IPv6 literal, use 1 and POSITIVE"
else
    fail "AddDomainNameServer refused the IPv6 literal $PROBE_DNS6"
    grep "^add6 " "$REPORT" >&2 || true
fi

if grep -q "^add6 listed rc 0 use 2$" "$REPORT"; then
    pass "a second Add on the listed address nests it to 2"
else
    fail "adding the listed IPv6 address again did not nest"
    grep "^add6 listed" "$REPORT" >&2 || true
fi

REMOVES6=$(sed -n "s/^remove6 listed rc [-0-9]* use \([-0-9]*\)$/\1/p" \
           "$REPORT" | tr '\n' ' ')
if [ "$REMOVES6" = "1 0 " ]; then
    pass "two Removes take the listed IPv6 address out, one at a time"
else
    fail "removing the listed IPv6 address gave use [$REMOVES6], expected 1 0"
    echo "       this is the row: a server the list reports and nothing" >&2
    echo "       can take away again." >&2
    grep "^remove6 listed" "$REPORT" >&2 || true
fi

if grep -q "^remove6 listed again rc -1 errno 2$" "$REPORT"; then
    pass "one Remove too many is -1/ENOENT for IPv6 as it is for IPv4"
else
    fail "the extra IPv6 Remove did not answer -1/ENOENT"
    grep "^remove6 listed again" "$REPORT" >&2 || true
fi

if grep -q "^add6 malformed rc -1 errno 22$" "$REPORT"; then
    pass "text that is neither address form is still -1/EINVAL"
else
    fail "a malformed address was not refused with EINVAL"
    grep "^add6 malformed" "$REPORT" >&2 || true
fi

if grep -q "^dnslist final6: $STATIC_DNS use -1 " "$REPORT" &&
   ! grep -q "^dnslist final6: .*:.* use " "$REPORT"; then
    pass "the list ends with the IPv4 entry alone: the probe put back what it took"
else
    fail "the IPv6 phase did not leave the name server list as it found it"
    grep "^dnslist final6:" "$REPORT" >&2 || true
fi

QUALIFIED=$(sed -n "s/^qualified \"$HOST\.$DOMAIN\": \(.*\)$/\1/p" "$REPORT" | head -1)
BARE=$(sed -n "s/^bare \"$HOST\": \(.*\)$/\1/p" "$REPORT" | head -1)
CONTROL=$(sed -n "s/^control \"$HOST\": \(.*\)$/\1/p" "$REPORT" | head -1)

if [ "$QUALIFIED" = "FAILED" ] || [ -z "$QUALIFIED" ]; then
    fail "$HOST.$DOMAIN did not resolve at all"
    echo "       (the name server is $STATIC_DNS; can it resolve" >&2
    echo "        $HOST.$DOMAIN?)" >&2
elif [ "$CONTROL" != "FAILED" ]; then
    fail "the bare name resolved with NO default domain set ($CONTROL)"
    echo "       the control arm is what makes the next assertion mean" >&2
    echo "       anything; something else on this network answered." >&2
elif [ "$BARE" = "$QUALIFIED" ]; then
    pass "with the domain set, \"$HOST\" resolves, to $HOST.$DOMAIN's own address"
else
    fail "\"$HOST\" gave '$BARE', $HOST.$DOMAIN gave '$QUALIFIED'"
fi

if grep -q "^missing \"nosuchhost-aminetxduo\": FAILED$" "$REPORT"; then
    pass "a bare name with no answer under the domain either still fails"
else
    fail "a name that exists nowhere was resolved"
fi

if grep -q "^domain 200 chars: rc 1 roundtrip yes$" "$REPORT"; then
    pass "a 200-character default domain survives Set/GetDefaultDomainName"
else
    fail "the default domain store is still too small for the documented 255"
fi

if grep -q "^domain cleared: rc 0$" "$REPORT"; then
    pass "GetDefaultDomainName is FALSE once the domain is cleared"
else
    fail "GetDefaultDomainName did not answer FALSE for a cleared domain"
    grep "^domain cleared:" "$REPORT" >&2 || true
fi

echo
if [ "$FAILED" -ne 0 ]; then
    echo "dns: FAILED" >&2
    exit 1
fi

echo "dns: PASSED"
exit 0
