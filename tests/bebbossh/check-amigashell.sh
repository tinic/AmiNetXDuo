#!/usr/bin/env bash
#
# Score a login INTO the Amiga: bebbosshd's own shell channel.
#
#   tests/bebbossh/check-amigashell.sh <testhd-dir>
#
# The far end here is an AmigaDOS Shell, so there is no `stty` to ask and the
# question is a different one from the outbound arm's: does logging in give a
# working shell at all, does a remote command run, and does the prompt come
# back.  tests/bebbossh/check-term.sh is the one that measures terminal size,
# and it needs a Unix on the other end to do it.
#
# SPDX-License-Identifier: MIT

set -uo pipefail

HD="${1:?usage: check-amigashell.sh <testhd-dir>}"
REPORT="$HD/client.txt"
SERVER="$HD/server.txt"
FAIL=0

if [ ! -f "$REPORT" ]; then
    echo "no DH0:client.txt -- the guest never ran the command list."
    echo "-------------------------------------------------------------"
    echo "VERDICT: FAIL -- no run"
    exit 1
fi

want() {
    local label="$1" marker="$2" file="$3"
    if grep -q "$marker" "$file" 2>/dev/null; then
        printf '  %-34s yes\n' "$label"
    else
        printf '  %-34s NO\n' "$label"
        FAIL=1
    fi
}

echo "logging in to bebbosshd, from bebbossh, both in the guest:"
want "the server accepted the key"      "USERAUTH_SUCCESS"   "$SERVER"
want "a remote command ran"             "AMIGA-EXEC-OK"      "$REPORT"
want "the shell channel opened"         "opening shell channel" "$SERVER"
want "a Shell prompt came back"         ":>"                 "$REPORT"
want "a piped command ran in the Shell" "SHELL-SESSION-START" "$REPORT"

# The Shell's prompt arrives with its own colour sequences, which is the
# clearest single sign that what came back is a terminal and not a pipe.
echo ""
if grep -q $'\033\[3[0-9]m' "$REPORT" 2>/dev/null; then
    echo "  the prompt carries ANSI colour, so the Shell is writing to a terminal"
else
    echo "  no ANSI in the prompt -- the Shell may not think it has a terminal"
fi

echo ""
echo "  what the server logged about the channel:"
grep -E "opening shell channel|CHANNEL_SUCCESS for|starting task|ended task|terminating" \
     "$SERVER" 2>/dev/null | sed 's/^/    /' | head -12

echo ""
echo "  return codes: $(grep -o '^--- rc [0-9-]*' "$REPORT" | awk '{printf "%s ", $3}')"

echo "-------------------------------------------------------------"
if [ "$FAIL" = "0" ]; then
    echo "VERDICT: PASS -- login, remote command and Shell all work"
    exit 0
fi
echo "VERDICT: FAIL -- see above"
exit 1
