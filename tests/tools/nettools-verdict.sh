#!/usr/bin/env bash
# The verdict half of tests/tools/run-nettools.sh, in a file that can be
# sourced without an emulator.
# SPDX-License-Identifier: MIT

nettools_verdict() {
    local report="$1" hd="$2" peerlog="$3" peer="${4:-10.0.2.2}"
    local inbound="${5:-unknown}"
    local failed=0

    _nv_fail() { echo "nettools_fail=$1"; failed=1; }

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

    _nv_ok() { # regex label
        local rc; rc=$(_nv_rc "$1")
        if [ "$rc" = 0 ]; then echo "nettools_$2=rc0"
        else echo "nettools_$2=rc${rc:-absent}"; _nv_fail "$2_returned_${rc:-nothing}"; fi
    }
    _nv_refused() { # regex label
        local rc; rc=$(_nv_rc "$1")
        if [ -z "$rc" ]; then echo "nettools_$2=absent"; _nv_fail "${2}_never_ran"
        elif [ "$rc" = 0 ]; then echo "nettools_$2=rc0"
            _nv_fail "${2}_returned_0_for_something_that_cannot_have_worked"
        else echo "nettools_$2=rc$rc"; fi
    }
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

    _nv_file nc-echo.txt nc_echo "" "hello from the amiga"

    _nv_file nc-loopback.txt nc_loopback "" "hello from the amiga"

    case "$inbound" in
        yes) _nv_file nc-inbound.txt nc_inbound "" "hello from the host" ;;
        no)  echo "nettools_nc_inbound=unforwarded"
             echo "nettools_nc_inbound_hole=nothing_outside_could_reach_the_listener" ;;
        *)   echo "nettools_nc_inbound=unknown"
             echo "nettools_nc_inbound_hole=the_harness_did_not_say_whether_the_forward_was_there" ;;
    esac

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

        local telnet_answers
        telnet_answers=$(sed -n 's/.*answers: //p' "$peerlog" | head -1)
        echo "nettools_telnet_answers=\"$telnet_answers\""
        if [ "$telnet_answers" != \
             "DO ECHO, DO SGA, WONT TERMINAL-TYPE, WONT WINDOW-SIZE" ]; then
            _nv_fail telnet_option_negotiation_was_not_answered
        fi
    else
        echo "nettools_peerlog=missing"
        _nv_fail netpeer_wrote_no_log_so_half_the_evidence_is_gone
    fi

    _nv_ok "^===== SYS:traceroute $peer " traceroute
    if _nv_block "^===== SYS:traceroute $peer " |
            grep -qE '^ *1 .*[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+'; then
        echo "nettools_traceroute_hop1=answered"
    else
        echo "nettools_traceroute_hop1=star"
        _nv_fail traceroute_got_no_answer_from_a_host_that_answers
    fi

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
