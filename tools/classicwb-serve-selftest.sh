#!/usr/bin/env bash
#
# Which address tools/classicwb.sh reaches the guest on.
#
#   tools/classicwb-serve-selftest.sh [ROOT]
#
# The launcher's lease sniffer sees IPv4 and nothing else.  On a segment whose
# DHCP does not answer, the guest falls back to 169.254/16, takes a global
# address by SLAAC and serves on it -- and every URL the launcher printed named
# the link-local address, so served_check asked a machine off the segment for
# an address it can never route to and the launch exited 1 on a guest that was
# up.
#
# The two functions under test are read out of tools/classicwb.sh itself, so
# this cannot pass against a copy of them.  Needs no toolchain and no emulator.
#
# Output is key=value plus an exit code.
#
# SPDX-License-Identifier: MIT

set -uo pipefail

ROOT="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
SRC="$ROOT/tools/classicwb.sh"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

[ -f "$SRC" ] || { echo "classicwb_serve_selftest=no_$SRC"; exit 1; }

sed -n '/^serving_address6() {/,/^}/p'  "$SRC" >  "$WORK/fn.sh"
sed -n '/^serve_host_for() {/,/^}/p'    "$SRC" >> "$WORK/fn.sh"

for want in serving_address6 serve_host_for; do
    grep -q "^$want() {" "$WORK/fn.sh" || {
        echo "classicwb_serve_selftest=missing_$want"
        echo "  tools/classicwb.sh no longer defines $want() at column 0," >&2
        echo "  so this test read nothing out of it." >&2
        exit 1; }
done
# shellcheck source=/dev/null
. "$WORK/fn.sh"

bad=0
check() { # name ipv4 netstatus want
    local got
    got=$(serve_host_for "$2" "$3")
    if [ "$got" = "$4" ]; then
        printf '  ok: %s -> %s\n' "$1" "${got:-<none>}"
    else
        printf 'classicwb_serve_error=%s wanted %s got %s\n' \
               "$1" "${4:-<none>}" "${got:-<none>}" >&2
        bad=$((bad + 1))
    fi
}

# The shape ShowNetStatus really writes, taken off a live guest.
cat > "$WORK/dual.txt" <<'EOF'
Interface eth0 (DEVS:Networks/anxnet.device unit 0)
  state       online          link up
  address     192.168.1.123   netmask 255.255.255.0 (/24)
  hardware    00:80:10:49:b4:29
  address6    2607:f598:e1a8:4c00:280:10ff:fe49:b429/64 (advertised)
  address6    fe80::280:10ff:fe49:b429/64
  address6    2607:f598:e1a8:4c00::15e7/64
EOF

# The same guest on a segment whose DHCP never answered.
sed 's/192\.168\.1\.123   netmask 255\.255\.255\.0 (\/24)/169.254.13.7    netmask 255.255.0.0 (\/16)/' \
    "$WORK/dual.txt" > "$WORK/linklocal.txt"

# An IPv4-only stack: not one address6 line anywhere.
grep -v address6 "$WORK/dual.txt" > "$WORK/v4only.txt"

# Nothing routable: link-local on both protocols.
grep -v '2607:' "$WORK/linklocal.txt" > "$WORK/nothing.txt"

# A global address the stack will not answer on yet, ahead of one it will.
cat > "$WORK/tentative.txt" <<'EOF'
  address6    fe80::280:10ff:fe49:b429/64
  address6    2607:f598:e1a8:4c00:280:10ff:fe49:b429/64 (tentative)
  address6    2607:f598:e1a8:4c00::15e7/64 (deprecated)
  address6    2001:db8:1::5/64 (advertised)
EOF

check leased      192.168.1.123 "$WORK/dual.txt"      192.168.1.123
check leased_v4only 192.168.1.123 "$WORK/v4only.txt"  192.168.1.123
check no_lease    169.254.13.7  "$WORK/linklocal.txt" \
                  '[2607:f598:e1a8:4c00:280:10ff:fe49:b429]'
check no_address  ""            "$WORK/linklocal.txt" \
                  '[2607:f598:e1a8:4c00:280:10ff:fe49:b429]'
check unreachable 169.254.13.7  "$WORK/nothing.txt"   ''
check tentative   169.254.13.7  "$WORK/tentative.txt" '[2001:db8:1::5]'

printf 'classicwb_serve_selftest=%s\n' \
       "$([ "$bad" = 0 ] && echo pass || echo fail)"
printf 'checks=6 failed=%s\n' "$bad"
[ "$bad" = 0 ]
