#!/usr/bin/env bash
#
# Run the `fetch` command against real URLs, under FS-UAE on SLIRP.
#
#   tests/tls/run-fetch.sh [-m MODEL] [-t SECONDS] [-c CPU] [-k MHZ] [-b BUILDDIR]
#
# tools/amiberry-run.sh starts ONE executable with no arguments, and `fetch` is a
# command that takes arguments, so ToolsSmoke stands in the middle: it reads
# DH0:commands.txt and runs each line through SystemTagList() with the output
# redirected into DH0:tools.txt, which the harness prints back.  Same shape as
# the tools smoke run; only the command list and the staging differ.
#
# This is the TRAVELLER test.  tests/tls/run-api.sh proves tls.library works
# for a program written against it; this proves the command we ship uses it,
# over http: as well as https:, and that the failures a user will actually
# meet, a certificate for somebody else, a scheme nobody supports, are
# legible rather than mysterious.
#
# NOT A BASELINE, for the same reasons as run-api.sh: it depends on the
# internet, on FS-UAE's SLIRP NAT, on third parties' servers, and on
# certificates that rotate.
#
# The a2065.device driver is not ours to ship: point AMINETXDUO_A2065 at one,
# or drop a copy in build/a2065.device.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
MODEL=A1200
TIMEOUT=420
CPU=""
CLOCK=""
BUILD="${AMINETXDUO_BUILD:-build/tls}"

while getopts "m:t:c:k:b:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        c) CPU="$OPTARG" ;;
        k) CLOCK="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        *) echo "usage: $0 [-m model] [-t seconds] [-c cpu] [-k MHz] [-b builddir]" >&2; exit 2 ;;
    esac
done

SMOKE="$ROOT/$BUILD/src/tools/ToolsSmoke"
FETCH="$ROOT/$BUILD/src/tools/fetch"
ADDIF="$ROOT/$BUILD/src/tools/AddNetInterface"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"
TLS="$ROOT/$BUILD/src/tlslib/tls.library"
STORE="$ROOT/$BUILD/certificates"

for f in "$SMOKE" "$FETCH" "$ADDIF" "$BSD"; do
    [ -f "$f" ] || { echo "missing $f, build the tree first" >&2; exit 2; }
done

# tls.library and the trust store are optional here on purpose: without them
# the https: lines must still fail LEGIBLY, and that is worth being able to
# run.  With them, they must succeed.
HAVE_TLS=0
if [ -f "$TLS" ] && [ -f "$STORE" ]; then
    HAVE_TLS=1
fi

# ------------------------------------------------------------- a2065 -----

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

# ------------------------------------------------------------- staging ---

STAGE="$ROOT/build/fetch-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$A2065" "$STAGE/devs/a2065.device"
cp "$BSD"   "$STAGE/libs/bsdsocket.library"
cp "$FETCH" "$STAGE/fetch"
cp "$ADDIF" "$STAGE/AddNetInterface"

if [ "$HAVE_TLS" = "1" ]; then
    cp "$TLS"   "$STAGE/libs/tls.library"
    cp "$STORE" "$STAGE/devs/Internet/certificates"
    echo "==> tls.library staged, trust store $(wc -c < "$STORE" | tr -d ' ') bytes"
else
    echo "==> NO tls.library in $BUILD, the https: lines must fail legibly"
fi

# One interface up front, so the stack comes up once and stays up: every
# `fetch` after it finds bsdsocket.library already open.  Without this each
# command would start and stop the whole stack, DHCP lease included.
#
# The URLs:
#   example.com        the plainest 200 there is, over http:
#   tls-v1-2.badssl.com  answers 301 to an absolute URL on a NON-DEFAULT PORT,
#                      which exercises the redirect follow and the ":1012" in
#                      the URL parser at the same time
#   ecc256.badssl.com  an ECDSA leaf, so the P-256 verify path is covered too
#   wrong.host.badssl.com  a valid chain for somebody else, must be REFUSED
#   ftp://             a scheme this command does not do
#
# DELIBERATELY ABSENT: https://example.com/, https://www.iana.org/ and every
# other Cloudflare-fronted host.  They send three- and four-certificate chains,
# which at 14 MHz take longer to verify than their front end is willing to wait
# for a ClientKeyExchange, so they fail with "the connection is closed" through
# no fault of ours and would make this run look broken.  Not a crash and not a
# library defect, that was the harness losing the EMULATOR to SIGPIPE; see
# docs/RESEARCH.md.  www.iana.org completes in 11.3 s at -k 28.
if [ -n "${AMINETXDUO_FETCH_COMMANDS:-}" ]; then
    cp "$AMINETXDUO_FETCH_COMMANDS" "$STAGE/commands.txt"
    echo "==> command list: $AMINETXDUO_FETCH_COMMANDS"
else
cat > "$STAGE/commands.txt" <<'EOF'
# the argument template, via ReadArgs' own "?"
SYS:fetch ?
SYS:AddNetInterface eth0
SYS:fetch http://example.com/
SYS:fetch http://example.com/ TO DH0:plain.txt
SYS:fetch https://tls-v1-2.badssl.com/ TO DH0:redirect.txt
SYS:fetch https://ecc256.badssl.com/ TO DH0:ecdsa.txt
SYS:fetch https://wrong.host.badssl.com/ TO DH0:refused.txt
SYS:fetch ftp://example.com/
SYS:fetch http://example.invalid/
EOF
fi

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-fetch}"

CPUARG=()
[ -z "$CPU" ]   || CPUARG+=(-c "$CPU")
[ -z "$CLOCK" ] || CPUARG+=(-k "$CLOCK")

set +e
"$ROOT/tools/amiberry-run.sh" -N a2065 -m "$MODEL" -t "$TIMEOUT" "${CPUARG[@]}" \
     "$SMOKE" "$STAGE/devs" "$STAGE/libs" "$STAGE/fetch" \
     "$STAGE/AddNetInterface" "$STAGE/commands.txt"
RUN_RC=$?
set -e

# ---------------------------------------------------------- the verdict ---
#
# This used to end in `exec <runner>`, so the script's exit status was
# ToolsSmoke's, which is 0 whenever the last command in the list returned 0.
# A `fetch` that refused everything, or a boot that never reached the network,
# was a pass.  The transcript is read instead, and the assertions are the ones
# the command list above was written for: a name that must resolve, a chain
# that must be REFUSED, and a scheme that must be turned down.
#
# Only asserted for the default command list: AMINETXDUO_FETCH_COMMANDS is a
# different run and nothing here knows what it asked for.
HD="$ROOT/build/amiberry-testhd-$AMINETXDUO_RUN_TAG"
REPORT="$HD/tools.txt"

if [ ! -s "$REPORT" ]; then
    echo "FAIL: the guest wrote no transcript at $REPORT (emulator rc=$RUN_RC)." >&2
    echo "      Nothing ran, so nothing about fetch was tested." >&2
    echo "fetch: FAILED (no transcript)" >&2
    exit 1
fi

echo
echo "===================== what the commands printed ====================="
cat "$REPORT"
echo "====================================================================="
echo

if [ -n "${AMINETXDUO_FETCH_COMMANDS:-}" ]; then
    echo "==> AMINETXDUO_FETCH_COMMANDS was set: there is no command list here"
    echo "    to score, so only the emulator's status was checked."
    [ "$RUN_RC" = "0" ] || {
        echo "fetch: FAILED, emulator rc=$RUN_RC" >&2
        printf 'name=fetch\nverdict=FAIL\nreason=emulator_rc\nrun_rc=%s\n' "$RUN_RC"
        exit 1; }
    # 77, NOT 0.  "The emulator came back" is not a result about fetch, and
    # printing `fetch: PASSED` for it made tests/tls/run-hangup.sh -- which
    # has no assertions of its own and forwards this exit status -- report a
    # pass for four rude-peer cases nothing had looked at.  77 is the skip
    # convention tools/test-verdict.sh already uses.
    echo "fetch: NOT SCORED (custom command list), read the transcript above"
    printf 'name=fetch\nverdict=SKIP\nreason=custom_command_list\nrun_rc=%s\n' \
           "$RUN_RC"
    exit 77
fi

FAILED=0
fail() { echo "FAIL: $*" >&2; FAILED=$((FAILED + 1)); }
pass() { echo "  ok: $*"; }

block() {
    awk -v want="===== $1 =====" '
        $0 == want { on = 1; next }
        on && /^----- rc / { print; exit }
        on { print }
    ' "$REPORT"
}
rc_of() { block "$1" | sed -n 's/^----- rc \([0-9-]*\),.*/\1/p'; }

want_rc() { # command expected description
    local got; got=$(rc_of "$1")
    if [ "$got" = "$2" ]; then pass "$3 (rc $got)"
    else fail "$3: '$1' returned '${got:-nothing}', not $2"
         block "$1" | sed 's/^/       /' >&2
    fi
}

# Every command in the list has to have run: ToolsSmoke says so itself.
if grep -q "^===== done, 0 command(s) would not run" "$REPORT"; then
    pass "every command in the list ran"
else
    fail "the run did not reach the end of the command list"
    grep -n "^===== done" "$REPORT" | sed 's/^/       /' >&2
fi

want_rc "SYS:AddNetInterface eth0" 0 "the interface came up"
want_rc "SYS:fetch http://example.com/" 0 "http://example.com/ was fetched"

# A chain that is valid for somebody else must be refused, not fetched.  This
# is the assertion the whole file exists for: a verifier that accepts it is
# worse than no TLS at all, and it returns 0 for every other line in the list.
if [ "$HAVE_TLS" = "1" ]; then
    want_rc "SYS:fetch https://ecc256.badssl.com/ TO DH0:ecdsa.txt" 0 \
            "an ECDSA leaf verified"
    if [ "$(rc_of 'SYS:fetch https://wrong.host.badssl.com/ TO DH0:refused.txt')" = "0" ]; then
        fail "wrong.host.badssl.com was ACCEPTED: a chain issued for another
       name verified, which is the whole failure this file is against"
        block "SYS:fetch https://wrong.host.badssl.com/ TO DH0:refused.txt" |
            sed 's/^/       /' >&2
    else
        pass "wrong.host.badssl.com was refused"
    fi
else
    # No tls.library: the https: lines must fail legibly rather than crash.
    if [ "$(rc_of 'SYS:fetch https://ecc256.badssl.com/ TO DH0:ecdsa.txt')" = "0" ]; then
        fail "an https: URL succeeded with no tls.library staged"
    else
        pass "https: failed legibly with no tls.library"
    fi
fi

# A scheme this command does not do, and a name that does not exist: both must
# be turned down rather than reported as a fetch.
if [ "$(rc_of 'SYS:fetch ftp://example.com/')" = "0" ]; then
    fail "ftp:// was accepted by a command that does not do ftp"
else
    pass "ftp:// was turned down"
fi
if [ "$(rc_of 'SYS:fetch http://example.invalid/')" = "0" ]; then
    fail "http://example.invalid/ 'succeeded', a name that cannot resolve"
else
    pass "a name that does not exist was reported as a failure"
fi

echo
if [ "$FAILED" -ne 0 ]; then
    echo "fetch: FAILED ($FAILED)" >&2
    exit 1
fi
echo "fetch: PASSED"
exit 0
