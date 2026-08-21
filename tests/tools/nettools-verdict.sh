#!/usr/bin/env bash
#
# The verdict half of tests/tools/run-nettools.sh, in a file that can be
# sourced without an emulator.
#
#   . "$ROOT/tests/tools/nettools-verdict.sh"
#   nettools_verdict <transcript> <guest-DH0-directory> <netpeer-log> \
#                    <peer-address> <inbound-forward: yes|no>
#
# WHAT IT REPLACED
#
#   Nothing.  The harness ended `RC=$?` / `exit "$RC"` on ToolsSmoke's status,
#   and ToolsSmoke returns 0 whenever the last line of its list did, so a run
#   in which nc connected to nothing, telnet negotiated nothing and every tftp
#   transfer came back empty exited 0 and read as coverage for five commands.
#   No grep, no counter, no check of any kind.
#
# WHAT IT ASSERTS, AND WHY THESE
#
#   The transcript is the weakest witness here, because every one of these
#   commands prints something whether or not anything crossed the wire.  So
#   most of what is checked is BYTES, on one side or the other:
#
#     * the files the guest wrote to DH0:, by content and by length.  A tftp
#       GET of a 100000-byte file that produces a 0-byte file is a pass to
#       anything reading a return code.  Both ends of the block loop are
#       covered on purpose: exact.bin is four whole blocks and ends with an
#       EMPTY data block, big.bin is not a multiple of 512 and ends with a
#       short one.
#     * what NETPEER SAW.  The telnet negotiation is only visible from the
#       other end -- refusing an option correctly cannot be read off the
#       client's own output -- and the inbound connection is by definition
#       the peer's story.
#     * the failures.  A name that does not resolve, a port nothing listens
#       on and a file that is not there must all be reported as failures,
#       because a command that returns 0 for those returns 0 for everything.
#
# Output is key=value on stdout, one per fact, and the return code is the
# verdict.
#
# SPDX-License-Identifier: MIT

# nettools_verdict REPORT HD PEERLOG PEER -> 0 pass, 1 fail
nettools_verdict() {
    local report="$1" hd="$2" peerlog="$3" peer="${4:-10.0.2.2}"
    local inbound="${5:-unknown}"
    local failed=0

    _nv_fail() { echo "nettools_fail=$1"; failed=1; }

    # The first block whose banner matches a regex, and its return code.
    _nv_block() {
        awk -v want="$1" '
            /^===== / { on = ($0 ~ want) && !seen; if (on) seen = 1; next }
            on && /^----- / { exit }
            on { print }
        ' "$report"
    }
    _nv_rc() {
        awk -v want="$1" '
            /^===== / { on = ($0 ~ want) && !seen; if (on) seen = 1; next }
            on && /^----- rc / { print; exit }
        ' "$report" | sed -n 's/^----- rc \([0-9-]*\),.*/\1/p'
    }

    # A command that had to work.
    _nv_ok() { # regex label
        local rc; rc=$(_nv_rc "$1")
        if [ "$rc" = 0 ]; then echo "nettools_$2=rc0"
        else echo "nettools_$2=rc${rc:-absent}"; _nv_fail "$2_returned_${rc:-nothing}"; fi
    }
    # A command that had to be REFUSED.  rc 0 here is the failure that matters:
    # it is a command reporting success for something that did not happen.
    _nv_refused() { # regex label
        local rc; rc=$(_nv_rc "$1")
        if [ -z "$rc" ]; then echo "nettools_$2=absent"; _nv_fail "${2}_never_ran"
        elif [ "$rc" = 0 ]; then echo "nettools_$2=rc0"
            _nv_fail "${2}_returned_0_for_something_that_cannot_have_worked"
        else echo "nettools_$2=rc$rc"; fi
    }
    # A file the guest wrote, by exact length and optionally by content.
    _nv_file() { # name label want-bytes [needle]
        local f="$hd/$1" n
        if [ ! -f "$f" ]; then
            echo "nettools_$2=absent"; _nv_fail "${2}_was_never_written"; return
        fi
        n=$(wc -c < "$f" | tr -d ' ')
        echo "nettools_$2=${n}bytes"
        if [ -n "$3" ] && [ "$n" != "$3" ]; then
            _nv_fail "${2}_is_${n}_bytes_not_$3"
        fi
        if [ -n "${4:-}" ] && ! grep -qaF -- "$4" "$f"; then
            _nv_fail "${2}_does_not_contain_what_was_sent"
        fi
    }

    if [ ! -s "$report" ]; then
        echo "nettools_transcript=missing"
        _nv_fail no_transcript
        echo "nettools_result=FAIL"
        return 1
    fi
    echo "nettools_transcript=present"

    if grep -q '^===== done, 0 command(s) would not run' "$report"; then
        echo "nettools_command_list=complete"
    else
        echo "nettools_command_list=short"
        _nv_fail command_list_did_not_finish
    fi

    _nv_ok '^===== SYS:AddNetInterface eth0 =====$' interface

    # ---- nc as a client ---------------------------------------------------
    #
    # -z on a port that answers must say so, and the word is the proof: the
    # command returns 0 for a scan that found nothing on some paths.
    _nv_ok "^===== SYS:nc -z $peer [0-9]+ -v =====$" nc_scan_open
    if _nv_block "^===== SYS:nc -z $peer [0-9]+ -v =====$" | grep -qi 'open'; then
        echo "nettools_nc_scan_says=open"
    else
        echo "nettools_nc_scan_says=nothing"
        _nv_fail nc_scan_did_not_report_an_open_port
    fi
    _nv_refused "^===== SYS:nc -z $peer 1-2 " nc_scan_closed
    _nv_refused "^===== SYS:nc $peer 1 -v" nc_closed_port
    _nv_refused '^===== SYS:nc no\.such\.host\.invalid ' nc_bad_name

    # The echo server sends back what it was given, so these bytes made the
    # whole round trip through a stack on both sides.
    _nv_file nc-echo.txt nc_echo "" "hello from the amiga"

    # ---- nc as a server ---------------------------------------------------
    #
    # Guest to guest, which needs bind(), listen(), accept() and a half-close.
    _nv_file nc-loopback.txt nc_loopback "" "hello from the amiga"

    # And the connection that came from OUTSIDE -- IF anything could make one.
    # The forward is `uae_slirp_redir`, which is FS-UAE's option: Amiberry
    # ignores it and the harness's own probe finds nothing in LISTEN on the
    # host, so on that runner nothing can call in and the file is empty for a
    # reason that is not the command.  Scored when the forward is there, named
    # as a hole when it is not, and never silently either way.
    case "$inbound" in
        yes) _nv_file nc-inbound.txt nc_inbound "" "hello from the host" ;;
        no)  echo "nettools_nc_inbound=unforwarded"
             echo "nettools_nc_inbound_hole=nothing_outside_could_reach_the_listener" ;;
        *)   echo "nettools_nc_inbound=unknown"
             echo "nettools_nc_inbound_hole=the_harness_did_not_say_whether_the_forward_was_there" ;;
    esac

    # ---- telnet -----------------------------------------------------------
    #
    # Bytes both ways through a real IAC negotiation: the server's banner and
    # its echo have to reach DH0:telnet.txt, and the guest's line has to reach
    # the server.  Either half alone can be produced by a connection that did
    # nothing.
    _nv_file telnet.txt telnet "" "you said: amiga"
    if [ -s "$peerlog" ]; then
        echo "nettools_peerlog=present"
        if ! grep -q 'telnet .*connection from ' "$peerlog"; then
            echo "nettools_telnet_session=absent"
            _nv_fail the_telnet_server_saw_no_session
        elif ! grep -q "telnet .*line: 'amiga'" "$peerlog"; then
            echo "nettools_telnet_session=connected_and_silent"
            _nv_fail the_telnet_server_got_no_line_from_the_guest
        else
            echo "nettools_telnet_session=complete"
        fi

        # REPORTED, NOT GATED.  netpeer.py's handler returns the moment it
        # reads `quit` and drops whatever else was in that recv() buffer, so
        # an empty list here is not evidence that the client answered nothing.
        # It is worth printing, and it is not worth a red line until the two
        # ends are ordered.
        echo "nettools_telnet_answers=\"$(sed -n 's/.*answers: //p' "$peerlog" |
                                          head -1)\""
    else
        echo "nettools_peerlog=missing"
        _nv_fail netpeer_wrote_no_log_so_half_the_evidence_is_gone
    fi

    # ---- traceroute -------------------------------------------------------
    #
    # The peer answers this one itself, so it is a complete trace and the hop
    # has to be there.  The three that go through SLIRP's proxy are stars by
    # design (docs/RESEARCH.md 20) and are not scored.
    _nv_ok "^===== SYS:traceroute $peer " traceroute
    if _nv_block "^===== SYS:traceroute $peer " |
            grep -qE '^ *1 .*[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+'; then
        echo "nettools_traceroute_hop1=answered"
    else
        echo "nettools_traceroute_hop1=star"
        _nv_fail traceroute_got_no_answer_from_a_host_that_answers
    fi

    # ---- tftp -------------------------------------------------------------
    #
    # netpeer.py's file set: hello.txt is 49 bytes, big.bin 100000, exact.bin
    # 2048 and four whole blocks.
    _nv_file tftp-hello.txt tftp_hello 49 "Hello from the host."
    _nv_file tftp-big.bin   tftp_big   100000 ""
    _nv_file tftp-exact.bin tftp_exact 2048 ""
    _nv_refused '^===== SYS:tftp .* GET no\.such\.file' tftp_missing
    if [ -s "$peerlog" ] && grep -q 'from-amiga.txt' "$peerlog"; then
        echo "nettools_tftp_put=received"
    else
        echo "nettools_tftp_put=absent"
        _nv_fail the_tftp_server_never_saw_the_put
    fi

    # ---- whois ------------------------------------------------------------
    _nv_ok '^===== SYS:whois plain\.test ' whois_plain
    if _nv_block '^===== SYS:whois plain\.test ' | grep -q 'PLAIN.TEST'; then
        echo "nettools_whois_record=present"
    else
        echo "nettools_whois_record=absent"
        _nv_fail whois_returned_no_record
    fi

    if [ "$failed" -ne 0 ]; then
        echo "nettools_result=FAIL"
        return 1
    fi
    echo "nettools_result=PASS"
    return 0
}
