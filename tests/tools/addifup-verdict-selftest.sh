#!/usr/bin/env bash
#
# Prove tests/tools/addifup-verdict.sh can fail.
#
#   tests/tools/addifup-verdict-selftest.sh
#
# run-addifup.sh is the gate for "AddNetInterface never came back", the defect
# that shipped in 0.17.0 and 0.17.1.  It needs a Kickstart, a2065.device and
# three minutes, so it runs on one self-hosted runner and nowhere else, and
# until this file existed nothing could tell a gate that passes from a gate
# that cannot fail.  Its previous two assertions could not: see the note at
# the top of addifup-verdict.sh.
#
# The fixtures are the literal output of src/tools/shownetstatus.c, one per
# way the bring-up can end.  Needs nothing; under a second.
#
# SPDX-License-Identifier: MIT

set -uo pipefail
ROOT="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
. "$ROOT/tests/tools/addifup-verdict.sh"

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

# A lease that arrived.  shownetstatus.c:403,409,437-450 then :551,552,571.
cat > "$T/good" <<'EOF'

Interface eth0 (a2065.device unit 0)
  state       online          link up
  address     10.0.2.15       netmask 255.255.255.0 (/24)
  broadcast   10.0.2.255
  hardware    52:54:00:12:34:56
  mDNS        no
  mtu         1500 bytes
  configured  DHCP

Interfaces
Name            State    Link     Address
eth0            online   up       10.0.2.15
EOF

# AddNetInterface returned, no server answered.  shownetstatus.c:456 is the
# placeholder, :573 the "-" in the table.  This is the run the old assertions
# graded PASS: the header carries "Address" and the placeholder carries
# "address".
cat > "$T/nolease" <<'EOF'

Interface eth0 (a2065.device unit 0)
  state       offline         link unknown
  address     handed out by DHCP when the interface comes up
  configured  DHCP

Interfaces
Name            State    Link     Address
eth0            offline  ?        -
EOF

# DHCP did not answer and the stack assigned itself an RFC 3927 address.  Up,
# with an address, and the lease did not complete.  The old dotted-quad
# assertion passed on this too.
cat > "$T/linklocal" <<'EOF'

Interface eth0 (a2065.device unit 0)
  state       online          link up
  address     169.254.17.42   netmask 255.255.0.0 (/16)
  broadcast   169.254.255.255
  hardware    52:54:00:12:34:56
  configured  DHCP

Interfaces
Name            State    Link     Address
eth0            online   up       169.254.17.42
EOF

# The header and a dotted quad and nothing else: no interface came up at all.
# Both old assertions passed on this file.
cat > "$T/headeronly" <<'EOF'

Interfaces
Name            State    Link     Address
(none configured)

Routes
Destination      Gateway          Netmask          Flags  Interface
default          10.0.2.2         0.0.0.0          UG     eth0
EOF

# The stack is running inside another program, so the flags cannot be read.
# shownetstatus.c:571 prints "?" for the state.
cat > "$T/unreadable" <<'EOF'

Interfaces
Name            State    Link     Address
eth0            ?        ?        10.0.2.15
EOF

# Link down with an address still configured from the file.
cat > "$T/linkdown" <<'EOF'

Interfaces
Name            State    Link     Address
eth0            online   down     10.0.2.15
EOF

: > "$T/empty"

n=0; bad=0
case_() { # description expected-rc report [ifname]
    local what="$1" want="$2" report="$3" ifname="${4:-eth0}"
    local out rc
    out=$(addifup_verdict "$report" "$ifname" 2>&1); rc=$?
    n=$((n + 1))
    if [ "$rc" = "$want" ]; then
        printf 'ok   %-38s -> %s\n' "$what" "$rc"
    else
        printf 'FAIL %-38s -> %s, wanted %s\n' "$what" "$rc" "$want"
        bad=$((bad + 1))
    fi
    printf '%s\n' "$out" | sed 's/^/       | /'
}

case_ "a lease that arrived"            0 "$T/good"
case_ "no server answered"              1 "$T/nolease"
case_ "RFC 3927 fallback, not a lease"  1 "$T/linklocal"
case_ "header and a route, no interface" 1 "$T/headeronly"
case_ "flags unreadable from here"      1 "$T/unreadable"
case_ "link down"                       1 "$T/linkdown"
case_ "an EMPTY transcript"             1 "$T/empty"
case_ "no transcript at all"            1 "$T/does-not-exist"
case_ "a good run, wrong interface"     1 "$T/good" eth1

# The two assertions this file replaced, run over the same fixtures, so the
# record shows what they graded rather than only asserting that they were
# wrong.  Both must pass every fixture above for this to mean anything.
echo
echo "-- what the superseded assertions graded --"
for f in good nolease linklocal headeronly unreadable linkdown; do
    old1=FAIL; old2=FAIL
    grep -qiE "online|address" "$T/$f" && old1=PASS
    grep -qE "[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+" "$T/$f" && old2=PASS
    printf '   %-12s old_state_check=%s old_address_check=%s\n' "$f" "$old1" "$old2"
done

echo
echo "addifup-verdict-selftest: $n cases, $bad wrong"
[ "$bad" -eq 0 ]
