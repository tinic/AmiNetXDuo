#!/usr/bin/env bash
# Run the `fetch` command against real URLs.
#
# BRIDGED.  The header said "under FS-UAE on SLIRP" and nothing had started
# fs-uae since 2026-08-04; what it actually did was call amiberry-run.sh with
# no backend, which is SLIRP, so every https: line was carried by the
# emulator's own TCP/IP rather than by this stack.
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
MODEL=A1200
TIMEOUT=420
CPU=""
CLOCK=""
BUILD="${AMINETXDUO_BUILD:-build/tls}"
IFACE="${AMINETXDUO_FETCH_IFACE:-${AMINETXDUO_AMIBERRY_BACKEND:-ens18}}"

while getopts "m:t:c:k:b:B:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        c) CPU="$OPTARG" ;;
        k) CLOCK="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        B) IFACE="$OPTARG" ;;
        *) echo "usage: $0 [-m model] [-t seconds] [-c cpu] [-k MHz] [-b builddir] [-B iface]" >&2; exit 2 ;;
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

HAVE_TLS=0
if [ -f "$TLS" ] && [ -f "$STORE" ]; then
    HAVE_TLS=1
fi


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
"$ROOT/tools/amiberry-run.sh" -N a2065 -B "$IFACE" -m "$MODEL" -t "$TIMEOUT" "${CPUARG[@]}" \
     "$SMOKE" "$STAGE/devs" "$STAGE/libs" "$STAGE/fetch" \
     "$STAGE/AddNetInterface" "$STAGE/commands.txt"
RUN_RC=$?
set -e

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

if grep -q "^===== done, 0 command(s) would not run" "$REPORT"; then
    pass "every command in the list ran"
else
    fail "the run did not reach the end of the command list"
    grep -n "^===== done" "$REPORT" | sed 's/^/       /' >&2
fi

want_rc "SYS:AddNetInterface eth0" 0 "the interface came up"
want_rc "SYS:fetch http://example.com/" 0 "http://example.com/ was fetched"

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
    if [ "$(rc_of 'SYS:fetch https://ecc256.badssl.com/ TO DH0:ecdsa.txt')" = "0" ]; then
        fail "an https: URL succeeded with no tls.library staged"
    else
        pass "https: failed legibly with no tls.library"
    fi
fi

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
