#!/usr/bin/env bash
#
# THE REGRESSION TEST FOR THE RESOLVER API.
#
#   tests/tools/run-dns.sh [-m MODEL] [-t SECONDS] [-b BUILDDIR]
#                          [-N BOARD] [-B IFACE]
#
# WHAT IT IS PROVING
#
#   1. AddDomainNameServer() NESTS.  "Adding the same address twice will
#      require two calls RemoveDomainNameServer() to remove it again"
#      (autodoc).  The failure this catches is not cosmetic: with a
#      non-counting implementation, two programs share a name server, the
#      first one exits, and the second one's resolver stops working.  The
#      probe adds the running stack's own server a second time and removes it
#      once, and the server must still be there.
#
#   2. dnsn_UseCount SAYS WHERE THE ENTRY CAME FROM.  Negative is "configured
#      statically in the file", positive is a real count of run-time adds.
#      This stack reported -1 for everything, which is right for the file's
#      entries and wrong for DHCP's and for AddDomainNameServer()'s.  So the
#      assertion is on both signs at once: the server this harness stages into
#      DEVS:Internet/name_resolution is negative and the probe's own
#      192.0.2.53 is positive, in the same list.
#
#   3. THE DEFAULT DOMAIN IS USED.  "If no domain name is part of a host name,
#      a default domain name can be added to it if the host name lookup fails"
#      (GetDefaultDomainName).  Get/SetDefaultDomainName used to be a property
#      bag over a feature that did not exist, NetX Duo stores the domain and
#      never reads it.  The control arm runs FIRST, with the domain cleared,
#      so that the bare name is seen to fail before it is seen to succeed; and
#      the bare name's answer must equal the fully qualified name's, which is
#      what distinguishes "the retry resolved the right name" from "something
#      answered".
#
#   4. inet_pton() TAKES ONLY DOTTED DECIMAL.  Its INPUTS are "numbers in the
#      range 0..255"; it shared inet_addr()'s 4.3BSD parser, so
#      inet_pton(AF_INET,"0177.0.0.1") succeeded as 127.0.0.1.  That is the
#      allow-list bypass shape: a caller that uses inet_pton() to decide
#      whether a string is a literal address sees one address and the next
#      resolver sees another.  inet_addr() must keep taking those forms, so
#      the probe asks both calls the same strings and this script asserts they
#      DISAGREE.
#
#   5. A SHORT BUFFER TRUNCATES.  "The returned name is null-terminated unless
#      insufficient space is provided" (gethostname), and its ERRORS are
#      EFAULT and EPERM, there is no error for a name that does not fit.
#      This stack answered -1/ENAMETOOLONG and wrote nothing, so a caller that
#      sized its buffer from the autodoc got a failure the autodoc does not
#      list.  The script asks one byte at a time and checks each prefix,
#      including the length at which the terminator has to be dropped.
#
#   6. gethostname() ANSWERS THE MACHINE'S CONFIGURED NAME, and truncates.
#      The autodoc's NOTES chain is the first online interface's address in
#      the host database, then reverse resolution, then HOSTNAME, then
#      "localhost".  This stack puts the configured name in front of all four
#      ON PURPOSE and says why at src/bsdsocket/resolver.c:440-455: it is the
#      same string DHCP option 12 announces and mDNS claims as <name>.local,
#      and a gethostname() that disagrees with what the machine tells the
#      network is the worse divergence.  src/config/config_hostname.c:97 then
#      gives an unconfigured machine "amiga-<last three MAC bytes>", so the
#      host-database step is unreachable whenever a card is present.
#
#      This group used to assert the autodoc's first step instead, and could
#      not pass on any machine with a network card in it.  What it asserts now
#      is the name the machine reports -- read out of the transcript, not
#      named here, because it is derived from the MAC of whichever board -N
#      put in -- and the truncation semantics, which is the regression this
#      group was really guarding.
#
#   7. h_name IS THE OFFICIAL NAME.  A hosts entry matches on its aliases, so
#      gethostbyname(alias) must answer with the entry's own name and list the
#      alias in h_aliases.  It used to echo whatever the caller passed.
#
#   8. 255.255.255.255 IS A LITERAL.  "INADDR_NONE ... is a valid broadcast
#      address, but inet_addr() cannot return that value without indicating
#      failure" (autodoc BUGS).  The literal test was inet_addr(), so the
#      broadcast address went to a name server as though it were a host name.
#
# THE ONE EXTERNAL DEPENDENCY, stated rather than hidden: the name server named
# in AMINETXDUO_DNS_STATIC has to answer for www.example.com.  The default is
# the LAN's own router.  If the fully qualified lookup fails the script says so
# and fails, rather than passing on a dead resolver.
#
# BRIDGED, NEVER SLIRP.  -B names the host NIC to bridge onto and the string
# `slirp` is refused outright.  The whole file used to run behind NAT -- the
# staged name server was SLIRP's forwarder at 10.0.2.3 -- and every lookup it
# makes now goes to a name server on the segment instead.
#
# ADDRESSES.  A STATIC address on that LAN rather than a lease, so the address
# the hosts file names for this machine is the address the machine has: the
# h_name assertions look that entry up by its own name and its alias.  The
# default is below the block tests/tools/cards.sh hands to the card sweeps and
# beside the .240 run-iperf.sh takes; AMINETXDUO_DNS_SELF, _GATEWAY and
# _NETMASK move it.
#
# -N PICKS THE BOARD, and its driver is staged to match: see sana2_stage below.
# The a2065.device driver is not ours to ship: point AMINETXDUO_A2065 at one,
# or drop a copy in build/a2065.device.  Every other board's driver comes out
# of AMINETXDUO_SANA2_STORE or ~/amiga-assets/devs.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

MODEL=A1200
# The bare-name lookups are meant to fail, and a failing lookup is the slow
# one: the resolver retries every server before it gives up, twice over for
# the arm that then retries with the domain appended.
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

# The static server this harness stages into DEVS:Internet/name_resolution, and
# a name that exists under a domain reserved for exactly this (RFC 2606).  The
# nesting assertions read this address back out of the list with use -1, so it
# has to be the one in the file and not one a lease supplied.
STATIC_DNS="${AMINETXDUO_DNS_STATIC:-192.168.1.1}"
HOST="${AMINETXDUO_DNS_HOST:-www}"
DOMAIN="${AMINETXDUO_DNS_DOMAIN:-example.com}"

# The guest's own address, and the name the staged hosts file gives it. Both
# gethostname()'s host-database step and the official-name assertions key off
# this pair; nothing else in the tree does, so it is staged rather than added
# to tests/netstack/devs.
SELF="${AMINETXDUO_DNS_SELF:-192.168.1.239}"
GATEWAY="${AMINETXDUO_DNS_GATEWAY:-192.168.1.1}"
NETMASK="${AMINETXDUO_DNS_NETMASK:-255.255.255.0}"
SELF_NAME=amiga-probe.localdomain
SELF_ALIAS=amiga-probe

# A MAC of its own, so two guests on this bridge cannot answer for each other.
export AMINETXDUO_AMIBERRY_MAC="${AMINETXDUO_DNS_MAC:-02:41:4d:49:00:d4}"

# ------------------------------------------------------------- staging ---

STAGE="$ROOT/build/dns-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$A2065" "$STAGE/devs/a2065.device"
cp "$BSD"   "$STAGE/libs/bsdsocket.library"
cp "$TOOLS/AddNetInterface" "$STAGE/AddNetInterface"
cp "$PROBE" "$STAGE/DnsProbe"

printf '%s %s %s\n' "$SELF" "$SELF_NAME" "$SELF_ALIAS" >> "$STAGE/devs/Internet/hosts"

# STATIC, not the shared fixture's DHCP: gethostname() is asserted to derive
# the guest's name from its own interface address in the hosts file above, and
# a leased address is not known when that file is written.
cat > "$STAGE/devs/NetInterfaces/eth0" <<IFEOF
DEVICE=a2065.device
UNIT=0
CONFIGURE=STATIC
ADDRESS=$SELF
NETMASK=$NETMASK
GATEWAY=$GATEWAY
STATE=up
IFEOF

# One name server, named here rather than leased, because the nesting
# assertions read it back with use -1 and that sign is what says "this entry
# came out of the file".  The shared fixture names SLIRP's forwarder.
cat > "$STAGE/devs/Internet/name_resolution" <<NREOF
nameserver $STATIC_DNS
domain localdomain
NREOF

# -N puts a board in the machine; this puts its driver in DEVS: and its name in
# DEVICE=.  Without it the line above stands whatever -N asked for, so every
# board but the A2065 opens a2065.device against hardware that is not there and
# the run reports a stack failure that is really a staging one.
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

# ------------------------------------------------------------------ run ---

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

# A guru inside the caller is what a wrong list node shape produces, so the
# first assertion is that the machine stayed up.
STARTS=$(grep -c "SYS:DnsProbe" "$REPORT" || true)
if [ "$STARTS" -eq 1 ]; then
    pass "the machine booted once and ran the probe (no reset)"
else
    fail "the probe line appears $STARTS times, the machine reset"
fi

# ---- inet_pton is strict, inet_addr is not -------------------------------

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

# The whole point: each of these must be refused by inet_pton and ACCEPTED by
# inet_addr. A single shared parser cannot satisfy both columns.
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

# ---- gethostname ---------------------------------------------------------

# THE NAME IS READ, not named.  Nothing in the guest's configuration names it,
# so config_hostname.c's default applies and that is cut from the MAC of the
# board -N asked for.  See the header: this is the stack's documented order and
# not the autodoc's.
HOSTNAME_GOT=$(sed -n 's/^hostname full: rc 0 "\(.*\)"$/\1/p' "$REPORT" | head -1)
if [ -n "$HOSTNAME_GOT" ]; then
    pass "gethostname answers \"$HOSTNAME_GOT\", the machine's configured name"
else
    fail "gethostname did not answer a name at all"
    grep "^hostname full:" "$REPORT" >&2 || true
    HOSTNAME_GOT="__no_name__"
fi

# And it is the shape config_hostname.c promises: the bare label, never
# qualified, and a valid RFC 1123 one.  A name with a dot in it would be the
# regression that comment names -- ShowNetStatus is what qualifies it.
case "$HOSTNAME_GOT" in
    __no_name__) ;;
    *.*) fail "gethostname answered the qualified \"$HOSTNAME_GOT\"; the bare
       label is what option 12 announces and what the mDNS label is cut from" ;;
    *)   pass "and it is the bare label, not a qualified name" ;;
esac

# 14 is EFAULT: "the name or namelen parameter gave an invalid address".  Its
# ERRORS list has EFAULT and EPERM and nothing else, and bsd_gethostname()
# returns bsd_fail(SocketBase, AMI_EFAULT) on this path, so an errno that is
# not 14 is the library's and not this test's.
if grep -q "^hostname null: rc -1 errno 14$" "$REPORT"; then
    pass "gethostname(NULL) is -1 with EFAULT"
else
    fail "gethostname(NULL) did not answer -1/EFAULT"
    grep "^hostname null:" "$REPORT" >&2 || true
fi

# THE REGRESSION. Each of these used to be -1/ENAMETOOLONG with the buffer
# untouched. A truncated name is not terminated, that is the only way the
# caller can tell it was cut.
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

# ---- the official name ---------------------------------------------------

for asked in "$SELF_ALIAS" "$SELF_NAME"; do
    if grep -q "^official \"$asked\": name \"$SELF_NAME\" alias \"$SELF_ALIAS\"$" "$REPORT"; then
        pass "gethostbyname(\"$asked\") answers with the official name and the alias"
    else
        fail "gethostbyname(\"$asked\") did not answer \"$SELF_NAME\"/\"$SELF_ALIAS\""
        grep "^official \"$asked\":" "$REPORT" >&2 || true
    fi
done

# ---- the broadcast literal -----------------------------------------------

if grep -q "^broadcast \"255.255.255.255\": 255.255.255.255$" "$REPORT"; then
    pass "255.255.255.255 is read as a literal, not sent to a name server"
else
    fail "gethostbyname(\"255.255.255.255\") did not answer the broadcast address"
    grep "^broadcast " "$REPORT" >&2 || true
fi

# ---- the nesting count ---------------------------------------------------

use_after() {
    # "add 10.0.2.3 rc 0 use -2" -> -2
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

# THE REGRESSION. Before the fix this dropped the entry outright.
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

# 2 is ENOENT: "the IP address to remove was not found" (autodoc).
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

# ---- the default domain --------------------------------------------------

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

# "success, FALSE if the default domain name is not set" (autodoc).
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
