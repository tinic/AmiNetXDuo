#!/usr/bin/env bash
#
# Acknowledgement and window behaviour of one machine, measured from off it.
#
#   tests/perf/run-ackscope.sh -A TARGET [-C user@host] [-t SECONDS]
#                              [-p PORT] [-T TAG] [-o DIR] [-g "ARGS"]
#                              [-- command to drive the transfer ...]
#
# WHAT IT IS FOR
#
#   docs/BACKLOG.md: "run-fitzbench.sh cannot see an ACK or window problem".
#   Throughput harnesses report bytes and seconds, so a stack that
#   acknowledges badly and one that is merely slow read the same, and three
#   rows are stuck on that -- real X-Surf hardware trailing other stacks while
#   emulation says we lead, anxnet.device 5% behind cnet.device on the PCMCIA
#   card, and no measurement at all of a bare 2 MB A1200.  This captures the
#   transfer at the other end and reads the acknowledgements out of it:
#   tests/perf/ackscope.py, key=value, RESULT=pass or fail.
#
# NOTHING RUNS ON THE MACHINE UNDER TEST
#
#   Which is the point.  The A1200 in question has 2 MB and no Fast RAM, and
#   a capture running on it would perturb what it measured.  Every
#   acknowledgement it sends and every window it advertises is visible at the
#   other end of the connection, so the other end is where this looks.
#
# THE TARGET IS A NAME OR AN ADDRESS, RESOLVED EVERY RUN
#
#   -A takes either.  The real A1200 takes a DHCP lease, so ITS ADDRESS
#   CHANGES ON EVERY REBOOT and a cached one points at whatever took the
#   lease next -- on this LAN that would be the emulated e2e guest, whose
#   address IS stable because it has a reservation.  A run against the wrong
#   machine is worse than no run: it produces a confident number under
#   another machine's name.
#
#   So the name is resolved on the capture host at the start of every run,
#   both the name and the address it resolved to are printed and are in the
#   key=value output, and a name that does not resolve stops the run.
#
# WHERE IT CAPTURES, AND WHY THAT IS NOT ALWAYS THE PEER
#
#   -C is the machine that runs tcpdump; it defaults to the peer named by -H.
#   The two are the same for real hardware: the Amiga talks to playhouse3 and
#   playhouse3 captures.  They are NOT the same for an emulated guest --
#   Amiberry bridges onto the emulator host's NIC, so the emulator host sees
#   the guest's frames, while the peer has to be a third machine because a
#   frame the emulator host sends to the guest never returns to its own pcap
#   (docs/RESEARCH.md 63).  In this lab that is: capture on playhouse3, peer
#   on playhouse2, which has no tcpdump at all.
#
# HOW THE TRANSFER GETS DRIVEN
#
#   Three ways, and the script does not care which:
#
#     -- command   run it here while the capture is open.  This is how an
#                  emulated arm works: `-- tests/tools/run-iperf.sh ...`.
#     -t SECONDS   just hold the capture open.  Somebody -- or the machine's
#                  own httpd -- does the transfer meanwhile.  This is the
#                  real-hardware path and needs nothing installed there.
#     both         the command runs and the capture stays open at least -t.
#
# EXIT STATUS, WHICH IS THE PROJECT'S CONVENTION AND NOT A NEW ONE
#
#   0  RESULT=pass
#   1  RESULT=fail -- a gate went red, which is a measurement
#   2  this box cannot run this test: no capture host, no tcpdump, no python
#   3  the target did not answer.  The name did not resolve, or it resolved
#      and nothing of it reached the capture.  NOT a pass and NOT a fail:
#      install/test/run-workbench.sh uses 3 for "could not observe what it
#      exists to observe" and both 2 and 3 are read as skips.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

# shellcheck source=tests/perf/peercap.sh
. "$ROOT/tests/perf/peercap.sh"

# amiga-1200.local is the real machine's mDNS name and is the default because
# the address it will have is not knowable.  It is NOT reachable when the
# machine is off, and this script says so and exits 3 rather than waiting.
TARGET="${AMINETXDUO_ACKSCOPE_TARGET:-amiga-1200.local}"
CAPHOST="${AMINETXDUO_ACKSCOPE_CAPHOST:-}"
PEER="${AMINETXDUO_FITZ_PEER:-}"
SECS=0
PORT=""
TAG="${AMINETXDUO_RUN_TAG:-ackscope}"
OUT=""
GATES="${AMINETXDUO_ACKSCOPE_ARGS:-}"
# 160, not peercap.sh's 128.  This reads TCP OPTIONS -- the window scale above
# all, without which every window figure is wrong by a factor of up to 256 --
# and 20 bytes of LINUX_SLL2 plus a 60-byte IP header plus a 60-byte TCP
# header is 140.
SNAP="${AMINETXDUO_ACKSCOPE_SNAPLEN:-160}"

usage() { sed -n '3,10p' "$0" >&2; }

while getopts "A:C:H:t:p:T:o:g:s:h" opt; do
    case "$opt" in
        A) TARGET="$OPTARG" ;;
        C) CAPHOST="$OPTARG" ;;
        H) PEER="$OPTARG" ;;
        t) SECS="$OPTARG" ;;
        p) PORT="$OPTARG" ;;
        T) TAG="$OPTARG" ;;
        o) OUT="$OPTARG" ;;
        g) GATES="$OPTARG" ;;
        s) SNAP="$OPTARG" ;;
        h) usage; exit 0 ;;
        *) usage; exit 2 ;;
    esac
done
shift $((OPTIND - 1))

CAPHOST="${CAPHOST:-$PEER}"
[ -n "$CAPHOST" ] || {
    echo "ackscope: no capture host.  -C names the machine that runs" >&2
    echo "tcpdump; -H alone is enough when the peer is also that machine." >&2
    exit 2; }
[ -n "$TARGET" ] || { usage; exit 2; }
[ "$SECS" != 0 ] || [ "$#" -gt 0 ] || {
    echo "ackscope: nothing would drive a transfer.  Give -t SECONDS to hold" >&2
    echo "the capture open, or -- followed by a command to run while it is." >&2
    exit 2; }

OUT="${OUT:-$ROOT/build/ackscope-$TAG}"
rm -rf "$OUT"; mkdir -p "$OUT"

# ------------------------------------------------------------- the target --
#
# RESOLVED ON THE CAPTURE HOST, not here.  That is the machine whose pcap
# filter the address goes into and whose view of the LAN decides what the
# name means; resolving it on a Mac over a VPN would put a different address
# in the filter and capture nothing.  getent covers /etc/hosts, DNS and
# mDNS through nss-mdns in one call, which is what makes a .local name work.
resolve() { # name
    case "$1" in
        [0-9]*.[0-9]*.[0-9]*.[0-9]*) echo "$1"; return 0 ;;
    esac
    ssh -o ConnectTimeout=10 "$CAPHOST" \
        "getent ahostsv4 '$1' 2>/dev/null | awk 'NR==1 { print \$1 }'" 2>/dev/null
}

ADDR=$(resolve "$TARGET" || true)
if [ -z "$ADDR" ]; then
    echo "target_name=$TARGET"
    echo "target_addr=unresolved"
    echo "RESULT=skip"
    echo "ackscope: $CAPHOST cannot resolve '$TARGET'." >&2
    echo "  A name that does not resolve is not a failed measurement, it is" >&2
    echo "  no measurement: the machine is off, or it has not announced" >&2
    echo "  itself yet.  Nothing here waits for it." >&2
    exit 3
fi
echo "target_name=$TARGET"
echo "target_addr=$ADDR"
echo "capture_host=$CAPHOST"
# driver_peer, NOT peer.  ackscope.py prints `peer=` for the address at the
# other end of the connection it read, which is the one that means something;
# a second `peer=` here with a different value -- an ssh target, or `none`
# when -H was not given -- puts two answers under one key in a stream whose
# whole contract is that a key has one.
echo "driver_peer=${PEER:-none}"

# A TARGET THAT IS THE CAPTURE HOST'S OWN ADDRESS IS A MISTAKE, not a run.
# It captures a loopback conversation and reports the host's own Linux stack
# as the Amiga's, which every number below would then be about.
SELF=$(ssh -o ConnectTimeout=10 "$CAPHOST" \
       "ip -4 -o addr show 2>/dev/null | awk '{ print \$4 }' | cut -d/ -f1" \
       2>/dev/null || true)
for a in $SELF; do
    [ "$a" != "$ADDR" ] || {
        echo "ackscope: $ADDR is $CAPHOST's own address.  '$TARGET' resolved" >&2
        echo "  to the capture host, so this would measure its stack and" >&2
        echo "  file the answer under the Amiga's name." >&2
        exit 2; }
done

# ------------------------------------------------------------ the capture --
#
# BY ADDRESS.  Every other caller of peercap.sh selects a port, which ties it
# to one workload; the machine under test keeps its address across every
# workload and keeps no port at all.
export AMINETXDUO_PEERCAP_FILTER="tcp and host $ADDR"
export AMINETXDUO_PEERCAP_SNAPLEN="$SNAP"

CAPTURING=0
# RETRIED FOR THE SAME REASON, and this end is the expensive one: the
# transfer has already happened, the collectors are still running on the
# capture host, and one failed scp throws the whole arm away.  Both halves are
# idempotent -- the kill is by a pid file that is removed, the copy overwrites
# -- so a second go costs nothing and recovers a capture that is sitting on
# the far side complete.
stop_capture() {
    [ "$CAPTURING" = 1 ] || return 0
    CAPTURING=0
    local n=0
    until peercap_stop "$CAPHOST" "$OUT" "$TAG"; do
        n=$((n + 1))
        [ "$n" -lt 3 ] || { echo "ackscope: gave up retrieving the capture" \
                                 "after $n attempts" >&2; return 0; }
        echo "==> retrieval attempt $n failed; retrying" >&2
        sleep $((n * 5))
    done
}
trap stop_capture EXIT INT TERM HUP

# RETRIED, BECAUSE ONE FAILED ssh IS NOT A VERDICT ON THE RIG.  peercap.sh
# asks the capture host what its tcpdump is over a single ssh with
# ConnectTimeout=10 and stderr discarded, so a connection that is refused or
# slow on a box with fifty logins and two emulators on it comes back as an
# empty answer and reads as "cannot tell what tcpdump is".  Seen three times
# on playhouse3 while another agent's Amiberry was booting, each time with
# the same tcpdump that worked a minute earlier and a minute later.  Losing a
# ten-minute arm to that is the expensive part.
#
# Three attempts, widening.  A capture host that is genuinely without tcpdump
# fails all three and the last attempt's diagnosis is the one printed, so a
# real absence still stops the run -- it just costs twenty seconds first.
ATTEMPT=0
until peercap_start "$CAPHOST" "${PORT:-0}" "$OUT" "$TAG"; do
    ATTEMPT=$((ATTEMPT + 1))
    [ "$ATTEMPT" -lt 3 ] || {
        echo "ackscope: could not start the capture on $CAPHOST in $ATTEMPT" >&2
        echo "  attempts.  The diagnosis above is the last one's." >&2
        exit 2; }
    echo "==> attempt $ATTEMPT to reach $CAPHOST failed; retrying" >&2
    sleep $((ATTEMPT * 5))
done
echo "capture_attempts=$((ATTEMPT + 1))"
CAPTURING=1

RC_DRIVE=0
START=$(date +%s)
if [ "$#" -gt 0 ]; then
    echo "==> driving: $*"
    "$@" > "$OUT/drive.txt" 2>&1 || RC_DRIVE=$?
    echo "drive_rc=$RC_DRIVE"
    [ "$RC_DRIVE" = 0 ] || {
        echo "==> the driver exited $RC_DRIVE; its output ends:" >&2
        tail -12 "$OUT/drive.txt" >&2 || true; }
fi
if [ "$SECS" != 0 ]; then
    # The remainder, not the whole of -t: a driver that already took longer
    # than the window has been driving the whole time, and sleeping again
    # after it would double the run for nothing.
    left=$(( SECS - ( $(date +%s) - START ) ))
    if [ "$left" -gt 0 ]; then
        echo "==> holding the capture open for $left more second(s)"
        # Slept in chunks so a Ctrl-C is taken promptly and the trap above
        # gets to stop the collectors rather than leaving them on the peer
        # for the 1800 s ceiling: peercap.sh's header records five orphaned
        # loops, the oldest five days old, found filling a tmpfs.
        while [ "$left" -gt 0 ]; do
            sleep $(( left > 5 ? 5 : left ))
            left=$(( SECS - ( $(date +%s) - START ) ))
        done
    fi
fi

stop_capture
trap - EXIT INT TERM HUP

PCAP="$OUT/$TAG.pcap"
[ -s "$PCAP" ] || {
    echo "RESULT=skip"
    echo "ackscope: no capture came back from $CAPHOST." >&2
    exit 3; }
echo "capture_bytes=$(wc -c < "$PCAP" | tr -d ' ')"

# ------------------------------------------------------------ the reading --
#
# Exit 3 and not 1 when the target contributed nothing.  A capture with no
# segments of the target in it is the machine being off, or on another
# segment, or never having been asked to do anything -- none of which is a
# verdict on its acknowledgement behaviour, and a red one would be read as
# one.  ackscope.py returns 2 for that and this translates it.
RC=0
# shellcheck disable=SC2086
python3 "$ROOT/tests/perf/ackscope.py" "$PCAP" --guest "$ADDR" \
        --name "$TARGET" ${PORT:+--port "$PORT"} $GATES \
        > "$OUT/ackscope.txt" 2>"$OUT/ackscope.err" || RC=$?
cat "$OUT/ackscope.txt"
if [ "$RC" = 2 ]; then
    sed 's/^/    /' "$OUT/ackscope.err" >&2
    echo "RESULT=skip"
    echo "ackscope: $ADDR ($TARGET) said nothing in $(basename "$PCAP")." >&2
    echo "  The capture is $(wc -c < "$PCAP" | tr -d ' ') bytes, so tcpdump" >&2
    echo "  ran; what is missing is the machine.  Nothing here waits for it." >&2
    exit 3
fi
[ ! -s "$OUT/ackscope.err" ] || sed 's/^/    /' "$OUT/ackscope.err" >&2
echo "out_dir=$OUT"
exit "$RC"
