#!/usr/bin/env bash
#
# Prove tests/tools/bringup-verdict.sh can fail.
#
#   tests/tools/bringup-verdict-selftest.sh
#
# bringup_verdict() is the gate under every arm of the machine matrix --
# run-cpuspeed.sh, run-bigmem.sh -- and those arms need a ROM, a driver and
# half an hour of emulator.  Nothing else could tell an arm that passes from
# an arm that cannot fail, which is the state the whole CPU-speed class of
# defects lived in: no arm ever ran a fast CPU, so nothing about a delay loop
# calibrated for 14 MHz was ever asserted, and the matrix was green throughout.
#
# The fixtures are the literal output of tests/netstack/netstack_test.c, one
# per way a bring-up can end on a machine that is faster than the one the
# delays were written for.  Needs nothing; under a second.
#
# THE FIXTURE THAT MATTERS is `linknoping`: link up, an address, and the
# gateway never answers.  A check count passes it -- the guest ran its twelve
# -- and it is precisely a card that was claimed too fast to work.
#
# SPDX-License-Identifier: MIT

set -uo pipefail
ROOT="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
. "$ROOT/tests/tools/bringup-verdict.sh"

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

# A machine that came all the way up.  netstack_test.c:364, :382, :397 and the
# pool line at :351.
cat > "$T/good" <<'EOF'
  ok   config is readable
  hostname 'amiga', 1 interface(s), 1 name server(s)
  ok   at least one interface is up
  ok   NX_IP exists
  ok   packet pool exists
  pool 84 packets (84 free) of 1592 bytes
  ok   pool is at least AMI_POOL_MIN_PACKETS
  ok   interface 0 link is up
  ok   adopted this Exec Task
  address 10.0.2.15
  gateway 10.0.2.2
  ok   interface 0 has an address
  ok   ICMP echo to loopback
  ok   ICMP echo to self
  ok   ICMP echo to gateway
  ok   DNS lookup
16 checks, 0 failures, PASS
EOF

# THE ONE THIS FILE IS FOR.  The card was claimed, DHCP completed, and nothing
# off the machine ever answers.  Every check that ran, ran; the count clears
# any floor; and the interface does not work.
cat > "$T/linknoping" <<'EOF'
  ok   config is readable
  ok   at least one interface is up
  ok   NX_IP exists
  ok   packet pool exists
  pool 84 packets (84 free) of 1592 bytes
  ok   pool is at least AMI_POOL_MIN_PACKETS
  ok   interface 0 link is up
  ok   adopted this Exec Task
  ok   interface 0 has an address
  ok   ICMP echo to loopback
  ok   ICMP echo to self
  FAIL ICMP echo to gateway (0xa000202)
  ok   DNS lookup
13 checks, 1 failures, FAIL
EOF

# No lease, so netstack_test never reaches the gateway check at all: the
# condition at :394 is false and the assertion is ABSENT rather than failed.
# A floor under the check count is what this fixture defeats -- twelve checks
# ran, and the wire was never touched.
cat > "$T/nolease" <<'EOF'
  ok   config is readable
  ok   at least one interface is up
  ok   NX_IP exists
  ok   packet pool exists
  pool 84 packets (84 free) of 1592 bytes
  ok   pool is at least AMI_POOL_MIN_PACKETS
  ok   interface 0 link is up
  ok   adopted this Exec Task
  address 0.0.0.0
  FAIL interface 0 has an address (0x0)
  ok   ICMP echo to loopback
  (no gateway configured, skipping the wire test)
12 checks, 1 failures, FAIL
EOF

# The card was never claimed.  This is what a settle loop that collapsed to
# nothing looks like from outside: the driver opened, the CIS walk or the
# reset window was not honoured, and the link never came up.
cat > "$T/nolink" <<'EOF'
  ok   config is readable
  ok   at least one interface is up
  ok   NX_IP exists
  ok   packet pool exists
  pool 84 packets (84 free) of 1592 bytes
  ok   pool is at least AMI_POOL_MIN_PACKETS
  FAIL interface 0 link is up (0x0)
  ok   adopted this Exec Task
  FAIL interface 0 has an address (0x0)
  ok   ICMP echo to loopback
12 checks, 2 failures, FAIL
EOF

# A machine with a lot of memory, so the pool arithmetic ran at a scale it has
# never run at.  Everything works and the pool is one packet: the shape
# tests/tools/run-bigmem.sh gates on, and invisible to every other assertion
# in this tree.
cat > "$T/tinypool" <<'EOF'
  ok   config is readable
  ok   at least one interface is up
  ok   NX_IP exists
  ok   packet pool exists
  pool 3 packets (3 free) of 1592 bytes
  ok   pool is at least AMI_POOL_MIN_PACKETS
  ok   interface 0 link is up
  ok   adopted this Exec Task
  ok   interface 0 has an address
  ok   ICMP echo to loopback
  ok   ICMP echo to self
  ok   ICMP echo to gateway
12 checks, 0 failures, PASS
EOF

# A guest that booted, printed its banner and stopped.  No check line at all.
cat > "$T/stopped" <<'EOF'
AmiNetXDuo netstack test
  ok   config is readable
EOF

: > "$T/empty"

n=0; bad=0
# BOTH HALVES, for the reason tests/tools/addifup-verdict-selftest.sh gives:
# the documented interface is `bringup_result=` on stdout, and it could drift
# apart from the return code with nothing to say so.
case_() { # description expected-rc expected-result transcript [poolmin]
    local what="$1" want="$2" wantr="$3" report="$4" poolmin="${5:-0}"
    local out rc gotr
    out=$(bringup_verdict "$report" "$poolmin" 2>&1); rc=$?
    gotr=$(printf '%s\n' "$out" | sed -n 's/^bringup_result=//p' | tail -1)
    n=$((n + 1))
    if [ "$rc" = "$want" ] && [ "$gotr" = "$wantr" ]; then
        printf 'ok   %-42s -> %s %s\n' "$what" "$rc" "$gotr"
    else
        printf 'FAIL %-42s -> %s %s, wanted %s %s\n' \
               "$what" "$rc" "$gotr" "$want" "$wantr"
        bad=$((bad + 1))
    fi
    printf '%s\n' "$out" | sed 's/^/       | /'
}

case_ "a machine that came all the way up"    0 PASS "$T/good"
case_ "link and lease, gateway never answers" 1 FAIL "$T/linknoping"
case_ "no lease, so the wire test never ran"  1 FAIL "$T/nolease"
case_ "the card was never claimed"            1 FAIL "$T/nolink"
case_ "a guest that stopped after one check"  1 FAIL "$T/stopped"
case_ "an EMPTY transcript"                   1 FAIL "$T/empty"
case_ "no transcript at all"                  1 FAIL "$T/does-not-exist"
case_ "a good run, pool floor satisfied"      0 PASS "$T/good"     8
case_ "a good run, pool BELOW the floor"      1 FAIL "$T/tinypool" 8

# What a check-count floor would have graded, over the same fixtures, so the
# record shows the gap rather than only asserting that there is one.  This is
# the assertion every existing tier-2 arm makes.
echo
echo "-- what a floor of 12 checks grades --"
for f in good linknoping nolease nolink tinypool stopped; do
    line=$(grep -aE '^[0-9]+ checks' "$T/$f" | tail -1)
    checks=$(printf '%s' "$line" | awk '{print $1}')
    fails=$(printf '%s' "$line" | awk '{print $3}')
    old=FAIL
    [ -n "${checks:-}" ] && [ "${checks:-0}" -ge 12 ] && [ "${fails:-1}" = 0 ] && old=PASS
    printf '   %-12s checks=%-4s failures=%-4s floor_verdict=%s\n' \
           "$f" "${checks:-none}" "${fails:-none}" "$old"
done

echo
echo "bringup-verdict-selftest: $n cases, $bad wrong"
[ "$bad" -eq 0 ]
