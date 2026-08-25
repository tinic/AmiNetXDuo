#!/usr/bin/env bash
#
# Score an interactive BebboSSH run: does the remote end see the terminal the
# Amiga console actually is?
#
#   tests/bebbossh/check-term.sh <testhd-dir> <results-dir>
#
# SPDX-License-Identifier: MIT

set -uo pipefail

HD="${1:?usage: check-term.sh <testhd-dir> <results-dir>}"
RES="${2:?usage: check-term.sh <testhd-dir> <results-dir>}"

REPORT="$HD/client.txt"
FAIL=0

if [ ! -f "$REPORT" ]; then
    echo "no DH0:client.txt, the guest never ran the command list."
    echo "-------------------------------------------------------------"
    echo "VERDICT: FAIL, no run"
    exit 1
fi

mapfile -t CONSIZES < <(sed -n 's/^--- console rows \([0-9]*\), cols \([0-9]*\)$/\1 \2/p' "$REPORT")
mapfile -t CONSPECS < <(sed -n 's/^--- input: \(CON:.*\)$/\1/p' "$REPORT")

echo "terminal size, as the Amiga console reports it against what the remote sees:"
printf '  %-28s %-14s %-14s %-14s %s\n' "window" "console says" "stty size" "tput cols/lines" ""

n=0
for arm in a b; do
    stty="$RES/$arm.stty"
    [ -f "$stty" ] || continue

    csize="${CONSIZES[$n]:-}"
    cspec="${CONSPECS[$n]:-?}"
    n=$((n + 1))

    crows="${csize%% *}"; ccols="${csize##* }"
    # stty size prints "rows cols"
    srows=$(awk '{print $1}' "$stty" 2>/dev/null)
    scols=$(awk '{print $2}' "$stty" 2>/dev/null)
    tcols=$(cat "$RES/$arm.cols" 2>/dev/null | tr -d '\n')
    tlines=$(cat "$RES/$arm.lines" 2>/dev/null | tr -d '\n')

    verdict="ok"
    if [ -z "$crows" ] || [ -z "$srows" ]; then
        verdict="INCOMPLETE"; FAIL=1
    elif [ "$crows" != "$srows" ] || [ "$ccols" != "$scols" ]; then
        verdict="MISMATCH"; FAIL=1
    fi

    printf '  %-28s %-14s %-14s %-14s %s\n' \
        "$(printf '%s' "$cspec" | cut -c1-28)" \
        "${crows}x${ccols}" "${srows}x${scols}" "${tcols}x${tlines}" "$verdict"
done

if [ "$n" = "0" ]; then
    echo "  (no console arm produced a result)"
    FAIL=1
fi

if [ "$n" -ge 2 ]; then
    if [ "${CONSIZES[0]:-x}" = "${CONSIZES[1]:-y}" ]; then
        echo ""
        echo "  !! both windows reported the same size, this run cannot tell a"
        echo "     correct implementation from a hardcoded one."
        FAIL=1
    fi
fi

echo ""
echo "the rest of the session:"

show() {
    local label="$1" file="$2"
    if [ -f "$file" ]; then
        printf '  %-24s %s\n' "$label" "$(tr -d '\n' < "$file" | cut -c1-60)"
    else
        printf '  %-24s (nothing)\n' "$label"
    fi
}
show "TERM as the host saw it" "$RES/a.term"
show "TIOCGWINSZ, window A"    "$RES/a.winsz"
show "TIOCGWINSZ, window B"    "$RES/b.winsz"
show "the pty device"          "$RES/a.tty"
show "no-pty arm (-T), stty"   "$RES/nopty.stty"
show "no-pty arm (-T), tty"    "$RES/nopty.tty"

if [ -f "$RES/a.stty_a" ]; then
    echo ""
    echo "the termios BebboSSH asked for, as the remote applied it:"
    for k in intr erase eof; do
        v=$(tr ';' '\n' < "$RES/a.stty_a" | tr ' ' '\n' | grep -A2 "^$k$" \
            | sed -n '3p')
        [ -n "$v" ] || v=$(sed -n "s/.*$k = \([^;]*\);.*/\1/p" "$RES/a.stty_a" | head -1)
        printf '  %-24s %s\n' "$k" "${v:-(not reported)}"
    done
    if grep -q 'erase = \^H' "$RES/a.stty_a"; then
        echo "  erase is ^H, which is BebboSSH's value and not the host default ^?"
    else
        echo "  !! erase is not ^H, the pty-req termios block did not take effect"
    fi
fi

# ---------------------------------------------------------------- resize ----
if [ -f "$RES/r1.stty" ] || [ -f "$RES/r2.stty" ]; then
    echo ""
    echo "resize, as the remote saw it:"
    before=$(cat "$RES/r1.stty" 2>/dev/null | tr -d '\n')
    after=$(cat "$RES/r2.stty" 2>/dev/null | tr -d '\n')
    conafter=$(sed -n 's/^--- console rows \([0-9]*\), cols \([0-9]*\)$/\1 \2/p' "$REPORT" | tail -1)
    printf '  %-24s %s\n' "the window itself" \
        "$(sed -nE 's/^--- window (before|after): +(.*)$/\1 \2/p' "$REPORT" | tr '\n' ' ')"
    printf '  %-24s %s\n' "before the resize" "${before:-(nothing)}"
    printf '  %-24s %s\n' "after the resize"  "${after:-(nothing)}"
    if [ -z "$before" ] || [ -z "$after" ]; then
        echo "  !! the resize arm did not produce both samples"
        FAIL=1
    elif [ "$before" = "$after" ]; then
        echo "  !! the remote size did not change, window-change was not sent,"
        echo "     or was sent and ignored.  A resized window is a resized pty."
        FAIL=1
    else
        echo "  the remote size changed, so window-change was sent and honoured"
    fi
fi

echo ""
ALL=$(cat "$REPORT" "$HD/server.txt" 2>/dev/null)
for marker in SESSION-OK SHELL-SESSION-START; do
    if printf '%s' "$ALL" | grep -q "$marker"; then
        printf '  %-24s seen %s time(s)\n' "$marker" "$(printf '%s' "$ALL" | grep -c "$marker")"
    else
        printf '  %-24s NOT SEEN\n' "$marker"
        FAIL=1
    fi
done

# Every command's return code, since a session that fails to start and one that
# ends badly look the same in the output alone.
echo ""
echo "  return codes: $(grep -o '^--- rc [0-9-]*' "$REPORT" | awk '{printf "%s ", $3}')"

echo "-------------------------------------------------------------"
if [ "$FAIL" = "0" ]; then
    echo "VERDICT: PASS, the remote end sees the console's real size"
    exit 0
fi
echo "VERDICT: FAIL, see above"
exit 1
