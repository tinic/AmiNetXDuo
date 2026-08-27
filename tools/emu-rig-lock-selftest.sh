#!/usr/bin/env bash
#
# Does tools/emu-rig-lock.sh actually keep two runs apart?
#
#   tools/emu-rig-lock-selftest.sh
#
# It needs no emulator, no ROM and no network, so it runs in the host stage
# beside the other selftests and goes red on a machine that cannot boot
# anything.
#
# SPDX-License-Identifier: MIT

set -uo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

# A lock directory of its own, so running this never disturbs a live run and a
# live run never fails this.
AMINETXDUO_RIG_LOCKDIR=$(mktemp -d "${TMPDIR:-/tmp}/rig-lock-selftest.XXXXXX")
export AMINETXDUO_RIG_LOCKDIR
trap 'rm -rf "$AMINETXDUO_RIG_LOCKDIR"' EXIT

# shellcheck source=emu-rig-lock.sh
. "$ROOT/tools/emu-rig-lock.sh"

# Nothing below can be evaluated without flock(1); every claim would refuse
# for the same reason and nine identical refusals prove nothing.  Exit 3 is
# this file's "could not be evaluated here".
if ! rig_have_flock; then
    echo "rig_lock_selftest=0 wrong unproven=all reason=no-flock(1)"
    echo "rig-lock selftest: flock(1) is not installed, so no claim on this"
    echo "  host can be taken or refused.  A host that runs emulator arms"
    echo "  needs util-linux's flock; a host that only builds does not."
    exit 3
fi

WRONG=0
ok()   { printf '  ok   %s\n' "$1"; }
bad()  { printf '  WRONG %s\n' "$1"; WRONG=$((WRONG + 1)); }

# --------------------------------------------------- 1. two claims differ --

rig_claim_port selftest-a || bad "the first claim failed outright"
A_PORT="${RIG_PORT:-}"
A_FD="${RIG_PORT_FD:-}"
RIG_PORT_FD=""            # keep it held; claim again in the same shell
rig_claim_port selftest-b || bad "the second claim failed outright"
B_PORT="${RIG_PORT:-}"

if [ -n "$A_PORT" ] && [ -n "$B_PORT" ] && [ "$A_PORT" != "$B_PORT" ]; then
    ok "two claims in one process: $A_PORT and $B_PORT"
else
    bad "two claims returned the same port ($A_PORT, $B_PORT)"
fi

rig_release_port
[ -n "$A_FD" ] && eval "exec ${A_FD}>&-" 2> /dev/null || true

# ------------------------------------- 2. a busy port is never handed out --

# Hold a listener on a port, mark it free in the lock directory, and check the
# allocator still refuses it.  This is the case the flock cannot see and the
# bind probe must.
if command -v python3 > /dev/null 2>&1; then
    LISTEN_OUT=$(mktemp "$AMINETXDUO_RIG_LOCKDIR/listener.XXXXXX")
    python3 -c '
import socket, sys, time
s = socket.socket()
s.bind(("127.0.0.1", 0))
s.listen(1)
sys.stdout.write("%d\n" % s.getsockname()[1])
sys.stdout.flush()
time.sleep(30)
' > "$LISTEN_OUT" &
    LISTEN_PID=$!
    BUSY=""
    for _ in 1 2 3 4 5 6 7 8 9 10; do
        BUSY=$(head -1 "$LISTEN_OUT" 2> /dev/null)
        [ -n "$BUSY" ] && break
        sleep 0.3
    done
    if [ -n "$BUSY" ]; then
        if rig_port_free "$BUSY"; then
            bad "rig_port_free says $BUSY is free while a listener holds it"
        else
            ok "a port with a listener on it is reported busy ($BUSY)"
        fi
        # And the allocator must skip it even when its lock file is free: the
        # single-port range below has exactly one candidate.
        if rig_claim_port selftest-busy "$BUSY" 1 2> /dev/null; then
            bad "the allocator handed out $BUSY with a listener on it"
            rig_release_port
        else
            ok "the allocator refuses a range whose only port is busy"
        fi
    else
        bad "could not start the stand-in listener"
    fi
    kill "$LISTEN_PID" 2> /dev/null
    wait "$LISTEN_PID" 2> /dev/null
else
    ok "no python3: the bind-probe assertions are not applicable here"
fi

# ------------------------------- 3. a claim dies with the claiming process --

CLAIM_OUT=$(mktemp "$AMINETXDUO_RIG_LOCKDIR/claim.XXXXXX")
(
    . "$ROOT/tools/emu-rig-lock.sh"
    rig_claim_port selftest-child > /dev/null 2>&1 &&
        echo "$RIG_PORT" > "$CLAIM_OUT"
    sleep 5
) &
CHILD=$!
HELD=""
for _ in 1 2 3 4 5 6 7 8 9 10; do
    HELD=$(head -1 "$CLAIM_OUT" 2> /dev/null)
    [ -n "$HELD" ] && break
    sleep 0.3
done

if [ -z "$HELD" ]; then
    bad "the child process never claimed a port"
else
    # While the child lives, that exact port must not be claimable.
    if rig_claim_port selftest-steal "$HELD" 1 2> /dev/null; then
        bad "claimed $HELD while another process held it"
        rig_release_port
    else
        ok "a held port cannot be claimed by a second process ($HELD)"
    fi
    pkill -P "$CHILD" 2> /dev/null
    kill "$CHILD" 2> /dev/null
    wait "$CHILD" 2> /dev/null
    # And once the holder is gone it must be claimable again, with no cleanup
    # step of any kind: that is what the kernel releasing flock buys.  Bounded
    # rather than instant, because a `sleep` the child had started INHERITS the
    # descriptor and holds the lock until it too is reaped -- which is exactly
    # why tools/amiberry-run.sh closes the lock in its reader subshell.
    RECLAIMED=0
    for _ in 1 2 3 4 5 6 7 8 9 10; do
        if rig_claim_port selftest-reclaim "$HELD" 1 2> /dev/null; then
            RECLAIMED=1
            rig_release_port
            break
        fi
        sleep 0.5
    done
    if [ "$RECLAIMED" = 1 ]; then
        ok "the port is free again once its holder is reaped"
    else
        bad "$HELD stayed locked after its holder died"
    fi
fi

# ---------------------------------------------- 4. named claims interlock --

NAME_OUT=$(mktemp "$AMINETXDUO_RIG_LOCKDIR/name.XXXXXX")
(
    . "$ROOT/tools/emu-rig-lock.sh"
    rig_claim_name selftest-exclusive "the-first-run" > /dev/null 2>&1 &&
        echo held > "$NAME_OUT"
    sleep 5
) &
NCHILD=$!
for _ in 1 2 3 4 5 6 7 8 9 10; do
    [ -s "$NAME_OUT" ] && break
    sleep 0.3
done

if [ ! -s "$NAME_OUT" ]; then
    bad "the child process never took the named claim"
else
    REFUSAL=$(rig_claim_name selftest-exclusive "the-second-run" 2>&1)
    RC=$?
    if [ "$RC" = 0 ]; then
        bad "two processes hold one named claim at once"
        rig_release_name selftest-exclusive
    elif printf '%s' "$REFUSAL" | grep -q "the-first-run"; then
        ok "the second run is refused and the refusal names the first"
    else
        bad "refused, but without naming the holder: $REFUSAL"
    fi
fi
pkill -P "$NCHILD" 2> /dev/null
kill "$NCHILD" 2> /dev/null
wait "$NCHILD" 2> /dev/null

FREED=0
for _ in 1 2 3 4 5 6 7 8 9 10; do
    if rig_claim_name selftest-exclusive "after" > /dev/null 2>&1; then
        FREED=1
        rig_release_name selftest-exclusive
        break
    fi
    sleep 0.5
done
if [ "$FREED" = 1 ]; then
    ok "the named claim is free again once its holder is reaped"
else
    bad "the named claim stayed held after its holder died"
fi

# -------------------------------------- 5. shared claims exclude a writer --

SHARED1=$(mktemp "$AMINETXDUO_RIG_LOCKDIR/shared1.XXXXXX")
SHARED2=$(mktemp "$AMINETXDUO_RIG_LOCKDIR/shared2.XXXXXX")
(
    . "$ROOT/tools/emu-rig-lock.sh"
    rig_claim_name_shared selftest-shared "shared-one" > /dev/null 2>&1 &&
        echo held > "$SHARED1"
    sleep 5
) &
S1PID=$!
(
    . "$ROOT/tools/emu-rig-lock.sh"
    rig_claim_name_shared selftest-shared "shared-two" > /dev/null 2>&1 &&
        echo held > "$SHARED2"
    sleep 5
) &
S2PID=$!
for _ in 1 2 3 4 5 6 7 8 9 10; do
    [ -s "$SHARED1" ] && [ -s "$SHARED2" ] && break
    sleep 0.3
done

if [ -s "$SHARED1" ] && [ -s "$SHARED2" ]; then
    ok "two shared users can hold one named claim"
else
    bad "the two shared users did not acquire together"
fi
if rig_claim_name selftest-shared "exclusive-during-shared" > /dev/null 2>&1; then
    bad "an exclusive user acquired over shared holders"
    rig_release_name selftest-shared
else
    ok "an exclusive user is refused while shared users hold the claim"
fi

kill "$S1PID" "$S2PID" 2> /dev/null
wait "$S1PID" 2> /dev/null
wait "$S2PID" 2> /dev/null
if rig_claim_name selftest-shared "exclusive-after-shared" > /dev/null 2>&1; then
    ok "the exclusive user acquires after shared users leave"
    rig_release_name selftest-shared
else
    bad "the shared claim stayed held after both users left"
fi

# ----------------------------------------- 6. an orphaned reader is found --
#
# And a shell that merely MENTIONS one is not.  The unanchored version of this
# pattern matched the `bash -c` wrapper around the test that was written to
# check it -- twice -- so a run would have refused to boot because somebody's
# ssh argument contained a port number.  Both directions are asserted here for
# that reason: a check that fires on everything is worse than no check.

R_PORT=19731
python3 -c 'import time; time.sleep(20)' \
        serial-timestamp.py 127.0.0.1 "$R_PORT" /dev/null &
R_PID=$!
bash -c "sleep 20 # python3 serial-timestamp.py 127.0.0.1 $R_PORT /dev/null" &
R_DECOY=$!
sleep 1

FOUND=$(rig_port_readers "$R_PORT")
if printf '%s' "$FOUND" | grep -q "^$R_PID "; then
    ok "an orphaned reader on a port is found ($R_PID)"
else
    bad "rig_port_readers missed the reader on $R_PORT: ${FOUND:-<nothing>}"
fi
if printf '%s' "$FOUND" | grep -q "^$R_DECOY "; then
    bad "rig_port_readers matched a shell that only mentions a reader ($R_DECOY)"
else
    ok "a shell that merely mentions one is not a reader"
fi
if [ -n "$(rig_port_readers $((R_PORT + 1)))" ]; then
    bad "rig_port_readers answered for a port nothing is aimed at"
else
    ok "a port with no reader aimed at it reports none"
fi
kill "$R_PID" "$R_DECOY" 2> /dev/null
wait "$R_PID" "$R_DECOY" 2> /dev/null

# -------------------------------------- 6. a live address is never claimed --

# 127.0.0.1 always answers, so a range of exactly that address must yield
# nothing.  This is the 192.168.1.243 case: an address free of other RUNS is
# not the same as an address free.
if ! ping -c 1 -W 1 127.0.0.1 > /dev/null 2>&1; then
    UNPROVEN=1
    echo "  SKIP the live-address assertion cannot be evaluated here:" \
         "unprivileged ICMP is off. As root:" \
         "sysctl -w net.ipv4.ping_group_range='0 2147483647'"
elif rig_claim_address 127.0.0 1 1 selftest-live 2> /dev/null; then
    bad "claimed 127.0.0.1, which answers a ping"
else
    ok "an address that answers a ping is not claimed"
fi

echo
if [ "$WRONG" = 0 ] && [ "${UNPROVEN:-0}" != 0 ]; then
    echo "rig_lock_selftest=0 wrong unproven=$UNPROVEN reason=the live-address\
 probe cannot run here, unprivileged ICMP is off and ping has no cap_net_raw"
    echo "rig-lock selftest: exclusive and released; the live-address probe"
    echo "  could not run on this host, so that one claim is UNPROVEN here."
    exit 3
fi
if [ "$WRONG" = 0 ]; then
    echo "rig_lock_selftest=0 wrong"
    echo "rig-lock selftest: every claim is exclusive, probed and released"
    exit 0
fi
echo "rig_lock_selftest=$WRONG wrong"
echo "rig-lock selftest: $WRONG assertions" >&2
exit 1
