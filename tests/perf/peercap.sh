#!/usr/bin/env bash
#
# Capture one FitzBench arm at the PEER, so tests/perf/lossrate.py has
# something to read.  Sourced by run-fitzbench.sh and run-stackprof.sh; not
# useful on its own.
#
#   peercap_start  <user@host> <port> <outdir> <tag>
#   peercap_stop   <user@host> <outdir> <tag>
#   peercap_report <outdir> <tag> [--max-loss PCT ...]
#
# WHY THE PEER AND NOT THE GUEST
#
#   The number wanted is how many of the peer's segments it had to send twice,
#   and the peer is where that is exact: it is the sender, so its own egress
#   is the whole population, no inference and nothing on the Amiga.  A capture
#   at the emulator's bridge would answer a different question, what
#   arrived, and would need the guest to be instrumented to answer this one.
#
# WHAT IS COLLECTED
#
#   - a pcap of the fitz port.  -s 128 is deliberate: the payload is not read,
#     and a full-length capture of a 4 MB transfer is 40 MB of disk for
#     nothing.  Payload LENGTHS come from the IP header, which is not
#     truncated, a short snaplen has previously been misread as short segments
#     and sent an investigation the wrong way.  128 rather than the 96 this
#     used to be: a cooked capture (`-i any`, the default here) carries a
#     20-byte LINUX_SLL2 header before the IP one, so 20 + 60 + 60 is the
#     worst case, and a segment whose TCP header is truncated is DROPPED by
#     lossrate.py rather than counted -- which biases the rate silently.
#   - an `ss -tim` poll of the same socket, because rwnd_limited,
#     sndbuf_limited and app_limited cannot be inferred from a capture at all.
#
# TCPDUMP NEEDS CAP_NET_RAW ON THE PEER, AND SUDO THERE NEEDS A PASSWORD.
# playhouse4 grants sudo for setcap and apt and nothing else, so `sudo tcpdump`
# hangs on a prompt no one can answer and -w captures nothing.  The pattern
# this repo already uses for `tc` in tests/perf/run-lossgate.sh:47 is a private
# copy of the binary carrying the capability, which leaves the packaged one
# alone:
#
#   cp $(command -v tcpdump) ~/tcpdump-cap
#   sudo /usr/sbin/setcap cap_net_raw,cap_net_admin+eip ~/tcpdump-cap
#
# That copy is the DEFAULT here, $HOME/tcpdump-cap, the way $HOME/tc-cap is the
# default for tc.  A peer that has not got one falls back to tcpdump on PATH
# and says so.
#
# A CAPABILITY IS DROPPED BY ANY WRITE TO THE FILE.  Re-copying the binary
# after a tcpdump upgrade leaves a file that is present, executable, the right
# version and unprivileged, and the only symptom is an empty capture.  So the
# copy is asked for its capability before the run rather than after: see
# peercap_tcpdump_state below.
#
#   AMINETXDUO_PEER_TCPDUMP   tcpdump on the peer  (default $HOME/tcpdump-cap)
#   AMINETXDUO_PEER_SS        ss on the peer               (default ss)
#   AMINETXDUO_PEER_IFACE     interface to capture         (default any)
#   AMINETXDUO_PEER_TMP       scratch directory on the peer (default /tmp)
#   AMINETXDUO_PEERCAP_MAX    seconds either collector may live (default 1800)
#   AMINETXDUO_LOSSRATE_ARGS  extra arguments for lossrate.py, so a gate it
#                             grows later needs no new letter here
#   AMINETXDUO_PEERCAP_FILTER the pcap filter, when a port is not what selects
#                             the traffic.  tests/perf/run-ackscope.sh selects
#                             by the guest's ADDRESS instead, because the
#                             machine under test keeps its address across
#                             workloads and does not keep a port.  Default is
#                             the port filter every other caller wants.
#   AMINETXDUO_PEERCAP_SNAPLEN  bytes kept per frame (default 128).  Raise it
#                             to read TCP OPTIONS: 20 LINUX_SLL2 + 60 IP + 60
#                             TCP is 140, and a truncated option is a window
#                             scale that silently reads as absent.
#
# SPDX-License-Identifier: MIT

PEERCAP_TCPDUMP="${AMINETXDUO_PEER_TCPDUMP:-\$HOME/tcpdump-cap}"
PEERCAP_TCPDUMP_NAMED=${AMINETXDUO_PEER_TCPDUMP:+yes}
PEERCAP_SS="${AMINETXDUO_PEER_SS:-ss}"
PEERCAP_IFACE="${AMINETXDUO_PEER_IFACE:-any}"
PEERCAP_TMP="${AMINETXDUO_PEER_TMP:-/tmp}"

# A CEILING OF THEIR OWN.  peercap_stop kills both collectors by PID, which is
# right and only runs when the harness reaches it: a run that is interrupted --
# a timeout, a Ctrl-C, an agent killed -- leaves them behind, and killing the
# ssh that started them does not touch them.  Found 2026-08-11 on playhouse4:
# FIVE `ss -tim` loops, the oldest five days old, each waking twenty times a
# second and appending to a file on a 2 GB tmpfs that was 100% full.  Two
# separate costs, and the second is the bad one: scp to that peer failed, and
# every throughput figure taken against it since had those loops running.
PEERCAP_MAX="${AMINETXDUO_PEERCAP_MAX:-1800}"

# What the peer's copy of tcpdump is: `ok`, `stripped`, `missing`, or `noexec`.
# Asked over one ssh, before anything is started, because every other way of
# finding out is a capture that turns out to be empty at the end of a run.
peercap_tcpdump_state() { # peer
    ssh -o ConnectTimeout=10 "$1" "
        t=$PEERCAP_TCPDUMP
        case \"\$t\" in
            */*) [ -f \"\$t\" ] || { echo missing; exit 0; }
                 [ -x \"\$t\" ] || { echo noexec; exit 0; } ;;
            *)   command -v \"\$t\" >/dev/null 2>&1 || { echo missing; exit 0; }
                 t=\$(command -v \"\$t\") ;;
        esac
        for g in /usr/sbin/getcap /sbin/getcap getcap; do
            command -v \$g >/dev/null 2>&1 || continue
            case \"\$(\$g \"\$t\" 2>/dev/null)\" in
                *cap_net_raw*) echo ok ;;
                *)             echo stripped ;;
            esac
            exit 0
        done
        echo nogetcap" 2>/dev/null
}

# The copy, checked, with the fallback and the two ways it can be wrong.
peercap_resolve_tcpdump() { # peer
    local peer="$1" state
    state=$(peercap_tcpdump_state "$peer")
    case "$state" in
        ok|nogetcap) return 0 ;;
        stripped)
            echo "peercap: $peer:$PEERCAP_TCPDUMP carries no CAP_NET_RAW," \
                 "so it would capture nothing." >&2
            case "$PEERCAP_TCPDUMP" in
                */*) echo "  A capability is dropped by ANY write to the file," \
                          "so a copy that was refreshed -- after a tcpdump" >&2
                     echo "  upgrade, say -- is present, executable, the right" \
                          "version and disarmed.  Set it again:" >&2
                     echo "    sudo /usr/sbin/setcap cap_net_raw,cap_net_admin+eip" \
                          "$PEERCAP_TCPDUMP" >&2 ;;
                *)   echo "  That is the packaged tcpdump, which is unprivileged" \
                          "everywhere.  Make a copy that is not:" >&2
                     echo "    cp \$(command -v tcpdump) ~/tcpdump-cap && sudo" \
                          "/usr/sbin/setcap cap_net_raw,cap_net_admin+eip ~/tcpdump-cap" >&2
                     echo "  and leave AMINETXDUO_PEER_TCPDUMP unset; that copy" \
                          "is the default." >&2 ;;
            esac
            return 1 ;;
        missing|noexec)
            if [ -n "${PEERCAP_TCPDUMP_NAMED:-}" ]; then
                echo "peercap: AMINETXDUO_PEER_TCPDUMP names" \
                     "$PEERCAP_TCPDUMP on $peer, and it is $state there" >&2
                return 1
            fi
            echo "peercap: no $PEERCAP_TCPDUMP on $peer, falling back to" \
                 "tcpdump on PATH; if that one is unprivileged, make the copy:" >&2
            echo "  cp \$(command -v tcpdump) ~/tcpdump-cap && sudo" \
                 "/usr/sbin/setcap cap_net_raw,cap_net_admin+eip ~/tcpdump-cap" >&2
            PEERCAP_TCPDUMP=tcpdump
            return 0 ;;
        *)
            echo "peercap: cannot tell what $peer:$PEERCAP_TCPDUMP is" \
                 "(the check said '${state:-nothing}')" >&2
            return 1 ;;
    esac
}

peercap_start() {
    local peer="$1" port="$2" outdir="$3" tag="$4"
    [ -n "$peer" ] || { echo "peercap: no peer, not capturing" >&2; return 1; }
    peercap_resolve_tcpdump "$peer" || return 1
    mkdir -p "$outdir"
    local filter="${AMINETXDUO_PEERCAP_FILTER:-tcp port $port}"
    local snap="${AMINETXDUO_PEERCAP_SNAPLEN:-128}"
    # The port the capture filter was set to, so peercap_report can tell
    # lossrate.py which connection to read.  Without it the report defaults to
    # 17712 and a run on any other port gets "no TCP on port 17712" -- a full
    # capture of the right traffic, read with the wrong filter.
    PEERCAP_PORT="$port"
    # The poll interval is 50 ms.  It is not a sampling rate for anything
    # timed, the capture carries the timing, it just has to be short
    # enough that the read phases contain samples at all.
    #
    # The `ss` poll needs a listening port to watch and there is not always
    # one: a capture selected by address covers whatever the machine does,
    # and `sport = :0` matches nothing while still writing a file every 50 ms
    # for the length of the run.  A port of 0 or none skips that collector,
    # and peercap_stop and lossrate.py both already treat the .ss file as
    # optional.
    local sscmd="true"
    [ -z "$port" ] || [ "$port" = 0 ] || sscmd="nohup timeout $PEERCAP_MAX sh -c 'while :; do
                         date +\"T %s.%N\"
                         $PEERCAP_SS -tim \"sport = :$port\"
                         sleep 0.05
                     done' > $PEERCAP_TMP/peercap-$tag.ss 2>/dev/null &
        echo \$! > $PEERCAP_TMP/peercap-$tag.ss.pid"
    ssh "$peer" "
        rm -f $PEERCAP_TMP/peercap-$tag.pcap $PEERCAP_TMP/peercap-$tag.ss
        nohup timeout $PEERCAP_MAX $PEERCAP_TCPDUMP -i $PEERCAP_IFACE -s $snap -w \
            $PEERCAP_TMP/peercap-$tag.pcap '$filter' \
            > $PEERCAP_TMP/peercap-$tag.tcpdump.log 2>&1 &
        echo \$! > $PEERCAP_TMP/peercap-$tag.tcpdump.pid
        $sscmd
        sleep 1
    " >/dev/null 2>&1 || { echo "peercap: could not start on $peer" >&2; return 1; }
    # tcpdump is nohup'd, so ssh returns 0 whatever became of it, and the one
    # thing it fails on here is the one thing that is not visible from the exit
    # status: opening the device needs CAP_NET_RAW and an unprivileged tcpdump
    # exits immediately with "You don't have permission to perform this capture
    # on that device".  Left unchecked the run captures nothing, peercap_stop
    # finds no file, the loss gate is skipped and the whole thing exits 0 --
    # a green -l 1.5 over an empty capture.  Ask the peer whether the process
    # is still there and what it said.
    local why
    why=$(ssh "$peer" "
        p=\$(cat $PEERCAP_TMP/peercap-$tag.tcpdump.pid 2>/dev/null)
        if [ -n \"\$p\" ] && kill -0 \$p 2>/dev/null &&
           [ -f $PEERCAP_TMP/peercap-$tag.pcap ]; then
            echo OK
        else
            cat $PEERCAP_TMP/peercap-$tag.tcpdump.log 2>/dev/null |
                grep -v '^tcpdump: listening' | head -3
        fi" 2>/dev/null)
    if [ "$why" != OK ]; then
        echo "peercap: tcpdump did not start on $peer:" >&2
        printf '%s\n' "${why:-no output from it at all}" | sed 's/^/  /' >&2
        echo "  it ran as $PEERCAP_TCPDUMP.  tcpdump needs CAP_NET_RAW;" >&2
        echo "  where sudo is not available, set the capability on a COPY:" >&2
        echo "  cp \$(command -v tcpdump) ~/tcpdump-cap && sudo" >&2
        echo "  /usr/sbin/setcap cap_net_raw,cap_net_admin+eip ~/tcpdump-cap" >&2
        return 1
    fi
    echo "==> capturing '$filter' at the peer into $PEERCAP_TMP/peercap-$tag.pcap"
    return 0
}

peercap_stop() {
    local peer="$1" outdir="$2" tag="$3"
    [ -n "$peer" ] || return 1
    # By PID, from the file written at start.  `pkill -f` matches the remote
    # shell's own command line and has killed other people's runs; `pkill -x
    # tcpdump` kills a capture somebody else started on the same machine.
    ssh "$peer" "
        for p in tcpdump ss; do
            f=$PEERCAP_TMP/peercap-$tag.\$p.pid
            [ -f \$f ] && kill \$(cat \$f) 2>/dev/null
            rm -f \$f
        done
        sleep 1
    " >/dev/null 2>&1 || true
    scp -q "$peer:$PEERCAP_TMP/peercap-$tag.pcap" "$outdir/$tag.pcap" \
        2>/dev/null || { echo "peercap: no capture came back" >&2; return 1; }
    scp -q "$peer:$PEERCAP_TMP/peercap-$tag.ss" "$outdir/$tag.ss" \
        2>/dev/null || true
    return 0
}

peercap_report() {
    local outdir="$1" tag="$2"
    shift 2
    local pcap="$outdir/$tag.pcap"
    # Nonzero, not zero.  A missing capture used to report itself and return
    # success, so a gate asked for with -l or -L passed by not running.
    [ -f "$pcap" ] || { echo "peercap: no $pcap to read" >&2; return 1; }
    local args=("$pcap")
    [ -z "${PEERCAP_PORT:-}" ] || args+=(--port "$PEERCAP_PORT")
    [ -f "$outdir/$tag.ss" ] && args+=(--ss "$outdir/$tag.ss")
    # shellcheck disable=SC2206
    [ -z "${AMINETXDUO_LOSSRATE_ARGS:-}" ] || args+=(${AMINETXDUO_LOSSRATE_ARGS})
    echo
    echo "==> inbound loss (tests/perf/lossrate.py, peer-side capture)"
    python3 "$(dirname "${BASH_SOURCE[0]}")/lossrate.py" "${args[@]}" "$@" \
        | sed 's/^/    /'
    return "${PIPESTATUS[0]}"
}
