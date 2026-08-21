#!/usr/bin/env bash
#
# Prove tests/tls/hangup-verdict.sh can fail.
#
#   tests/tls/hangup-verdict-selftest.sh
#
# run-hangup.sh needs a Kickstart, a driver, a rude peer and five minutes, and
# for its whole life it asserted nothing: it forwarded run-fetch.sh's status
# for a command list run-fetch.sh does not score.  A verdict that has never
# been shown a failing run is the same thing one step along, so every way the
# four cases can go wrong is here as a transcript, and each one has to come
# out red.
#
# The fixtures are ToolsSmoke's own framing (src/tools/toolssmoke.c:589 and
# the "----- rc N, M ms" line) with fetch's messages
# (src/tlslib/tls_conn.c:250-264) in them.  Needs nothing; under a second.
#
# SPDX-License-Identifier: MIT

set -uo pipefail
ROOT="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
# shellcheck source=tests/tls/hangup-verdict.sh
. "$ROOT/tests/tls/hangup-verdict.sh"

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

pass=0; fail=0
expect() { # name want-rc file [timeout] [peerlog]
    local name="$1" want="$2" file="$3" to="${4:-15}" pl="${5:-$T/peer-all}"
    local got out
    out=$(hangup_verdict "$file" "$to" "$pl"); got=$?
    if [ "$got" = "$want" ]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        echo "SELFTEST FAIL: $name wanted rc $want, got $got" >&2
        printf '%s\n' "$out" | sed 's/^/    /' >&2
    fi
}

head_ok() {
    cat <<'EOF'

===== SYS:AddNetInterface eth0 =====
eth0: a2065.device unit 0
eth0: starting the network...
eth0: online, address 192.168.1.181 and fe80::280:10ff:fe49:60cd
----- rc 0, 1760 ms, free 8886288 -----
EOF
}
tail_ok() { printf '\n===== done, 0 command(s) would not run =====\n'; }

# What hangup-server.py prints the moment it has a ClientHello.
cat > "$T/peer-all" <<'EOF'
ready
rst: 133 bytes from 127.0.0.1:1024
fin: 133 bytes from 127.0.0.1:1025
hang: 133 bytes from 127.0.0.1:1026
garbage: 133 bytes from 127.0.0.1:1027
EOF

fetch_block() { # url rc ms message
    printf '\n===== SYS:fetch %s QUIET =====\n' "$1"
    [ -z "$4" ] || printf 'fetch: %s\n' "$4"
    printf -- '----- rc %s, %s ms, free 8632720 -----\n' "$2" "$3"
}

# ---- the run this harness is written for ---------------------------------
{
    head_ok
    fetch_block "https://10.0.2.2:4443/" 10 2140 "10.0.2.2: the connection is closed"
    fetch_block "https://10.0.2.2:4444/" 10 2020 "10.0.2.2: the connection is closed"
    fetch_block "https://10.0.2.2:4445/" 10 15400 "10.0.2.2: the server stopped answering"
    fetch_block "https://10.0.2.2:4446/" 10 2260 "10.0.2.2: the connection is closed"
    tail_ok
} > "$T/good"
expect "four legible errors" 0 "$T/good"

# ---- the loudest failure there is ----------------------------------------
# A rude peer answered with 32 bytes of nonsense and the command reported a
# fetch.  This is the one the whole file exists against, and the status the
# harness used to forward would have been 0 for it.
{
    head_ok
    fetch_block "https://10.0.2.2:4443/" 10 2140 "10.0.2.2: the connection is closed"
    fetch_block "https://10.0.2.2:4444/" 10 2020 "10.0.2.2: the connection is closed"
    fetch_block "https://10.0.2.2:4445/" 10 15400 "10.0.2.2: the server stopped answering"
    printf '\n===== SYS:fetch https://10.0.2.2:4446/ QUIET =====\n'
    printf 'HTTP/1.1 200 OK\n32 bytes -> DH0:garbage.txt\n'
    printf -- '----- rc 0, 2260 ms, free 8632720 -----\n'
    tail_ok
} > "$T/accepted"
expect "nonsense reported as a fetch" 1 "$T/accepted"

# ---- the machine went down on the reset ----------------------------------
# Two blocks, and the transcript stops.  "A peer must not be able to take the
# machine down" is exactly this count.
{
    head_ok
    fetch_block "https://10.0.2.2:4443/" 10 2140 "10.0.2.2: the connection is closed"
    printf '\n===== SYS:fetch https://10.0.2.2:4444/ QUIET =====\n'
} > "$T/died"
expect "the guest stopped after the second case" 1 "$T/died"

# ---- the deadline was not served -----------------------------------------
# A peer that never answers has no event to end the wait, so an instant return
# is a TIMEOUT that did not wait...
{
    head_ok
    fetch_block "https://10.0.2.2:4443/" 10 2140 "10.0.2.2: the connection is closed"
    fetch_block "https://10.0.2.2:4444/" 10 2020 "10.0.2.2: the connection is closed"
    fetch_block "https://10.0.2.2:4445/" 10 40 "10.0.2.2: the server stopped answering"
    fetch_block "https://10.0.2.2:4446/" 10 2260 "10.0.2.2: the connection is closed"
    tail_ok
} > "$T/instant"
expect "hang returned in 40 ms" 1 "$T/instant"

# ... and a wait of several minutes is one nothing served.
{
    head_ok
    fetch_block "https://10.0.2.2:4443/" 10 2140 "10.0.2.2: the connection is closed"
    fetch_block "https://10.0.2.2:4444/" 10 2020 "10.0.2.2: the connection is closed"
    fetch_block "https://10.0.2.2:4445/" 10 240000 "10.0.2.2: the server stopped answering"
    fetch_block "https://10.0.2.2:4446/" 10 2260 "10.0.2.2: the connection is closed"
    tail_ok
} > "$T/forever"
expect "hang took four minutes" 1 "$T/forever"

# ---- an error nobody can act on ------------------------------------------
{
    head_ok
    fetch_block "https://10.0.2.2:4443/" 10 2140 ""
    fetch_block "https://10.0.2.2:4444/" 10 2020 "10.0.2.2: the connection is closed"
    fetch_block "https://10.0.2.2:4445/" 10 15400 "10.0.2.2: the server stopped answering"
    fetch_block "https://10.0.2.2:4446/" 10 2260 "10.0.2.2: the connection is closed"
    tail_ok
} > "$T/silent"
expect "a case that failed and said nothing" 1 "$T/silent"

# ---- a code that is not fetch's ------------------------------------------
# 20 is what a command returns when it did not get as far as understanding the
# failure, and it used to be indistinguishable from 10 to anything reading
# this transcript.
{
    head_ok
    fetch_block "https://10.0.2.2:4443/" 20 2140 "10.0.2.2: no socket (errno 55)"
    fetch_block "https://10.0.2.2:4444/" 10 2020 "10.0.2.2: the connection is closed"
    fetch_block "https://10.0.2.2:4445/" 10 15400 "10.0.2.2: the server stopped answering"
    fetch_block "https://10.0.2.2:4446/" 10 2260 "10.0.2.2: the connection is closed"
    tail_ok
} > "$T/rc20"
expect "a case that returned 20" 1 "$T/rc20"

# ---- the list did not finish ---------------------------------------------
{
    head_ok
    fetch_block "https://10.0.2.2:4443/" 10 2140 "10.0.2.2: the connection is closed"
    fetch_block "https://10.0.2.2:4444/" 10 2020 "10.0.2.2: the connection is closed"
    fetch_block "https://10.0.2.2:4445/" 10 15400 "10.0.2.2: the server stopped answering"
    fetch_block "https://10.0.2.2:4446/" 10 2260 "10.0.2.2: the connection is closed"
    printf '\n===== done, 2 command(s) would not run =====\n'
} > "$T/short"
expect "two commands never ran" 1 "$T/short"

# ---- the network never came up -------------------------------------------
{
    printf '\n===== SYS:AddNetInterface eth0 =====\n'
    printf 'AddNetInterface: eth0 did not come online\n'
    printf -- '----- rc 10, 30160 ms, free 8886288 -----\n'
    fetch_block "https://10.0.2.2:4443/" 10 40 "10.0.2.2: the network did not start"
    fetch_block "https://10.0.2.2:4444/" 10 40 "10.0.2.2: the network did not start"
    fetch_block "https://10.0.2.2:4445/" 10 40 "10.0.2.2: the network did not start"
    fetch_block "https://10.0.2.2:4446/" 10 40 "10.0.2.2: the network did not start"
    tail_ok
} > "$T/nonet"
expect "the interface never came up" 1 "$T/nonet"

# ---- nothing at all ------------------------------------------------------
: > "$T/empty"
expect "an empty transcript" 1 "$T/empty"
expect "a transcript that is not there" 1 "$T/missing"

# ---- the run a broken rig produces ---------------------------------------
# Nothing reached the peer, so all four connections timed out and `fetch`
# reported a legible timeout for each.  Every assertion about return codes and
# sentences holds and NOT ONE of the four behaviours was exercised.  This is
# the shape a SLIRP that only times out produces, and it is the reason the
# peer's own log is read at all.
{
    head_ok
    fetch_block "https://10.0.2.2:4443/" 10 30040 "10.0.2.2: the server stopped answering"
    fetch_block "https://10.0.2.2:4444/" 10 30040 "10.0.2.2: the server stopped answering"
    fetch_block "https://10.0.2.2:4445/" 10 15400 "10.0.2.2: the server stopped answering"
    fetch_block "https://10.0.2.2:4446/" 10 30040 "10.0.2.2: the server stopped answering"
    tail_ok
} > "$T/allnothing"
printf 'ready\n' > "$T/peer-none"
expect "four connections that reached nobody" 1 "$T/allnothing" 15 "$T/peer-none"

# ... and the same transcript with the peer log claiming it saw them, which is
# still red: a reset is not a timeout, and three of these say it was.
expect "three closes reported as timeouts" 1 "$T/allnothing"

# ---- one behaviour that never reached the peer ---------------------------
cat > "$T/peer-three" <<'EOF'
ready
rst: 133 bytes from 127.0.0.1:1024
fin: 133 bytes from 127.0.0.1:1025
hang: 133 bytes from 127.0.0.1:1026
EOF
expect "the garbage listener saw nothing" 1 "$T/good" 15 "$T/peer-three"

# ---- and with no peer log named at all, that half is simply not asserted --
expect "no peer log named" 0 "$T/good" 15 ""

echo "hangup-verdict selftest: $pass passed, $fail failed"
[ "$fail" = 0 ] || exit 1
exit 0
