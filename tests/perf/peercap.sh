#!/usr/bin/env bash
# Capture one FitzBench arm at the PEER, so tests/perf/lossrate.py has
# something to read.  Sourced by run-fitzbench.sh and run-stackprof.sh; not
# useful on its own.
# SPDX-License-Identifier: MIT

PEERCAP_TCPDUMP="${AMINETXDUO_PEER_TCPDUMP:-\$HOME/tcpdump-cap}"
PEERCAP_TCPDUMP_NAMED=${AMINETXDUO_PEER_TCPDUMP:+yes}
PEERCAP_SS="${AMINETXDUO_PEER_SS:-ss}"
PEERCAP_IFACE="${AMINETXDUO_PEER_IFACE:-any}"
PEERCAP_TMP="${AMINETXDUO_PEER_TMP:-/tmp}"

PEERCAP_MAX="${AMINETXDUO_PEERCAP_MAX:-1800}"

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
    PEERCAP_PORT="$port"
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
