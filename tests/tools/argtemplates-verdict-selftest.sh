#!/usr/bin/env bash
# Prove src/tools/test/test_argtemplates.c can fail.
# SPDX-License-Identifier: MIT

set -uo pipefail
ROOT="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

CHECKER="$T/test_argtemplates"
TREE="$T/tree"

cc -std=c11 -Wall -Wextra -D_GNU_SOURCE \
   "$ROOT/src/tools/test/test_argtemplates.c" -o "$CHECKER" || {
    echo "argtemplates selftest: cannot build the checker"
    exit 1
}

mkdir -p "$TREE/src/tools" "$TREE/docs/user"
cp "$ROOT"/src/tools/*.c "$TREE/src/tools/"
cp "$ROOT/docs/user/AmiNetXDuo.guide" "$TREE/docs/user/"

cases=0
wrong=0

replace() {
    python3 - "$TREE/$1" "$2" "$3" <<'PY'
import io, sys
path, old, new = sys.argv[1], sys.argv[2], sys.argv[3]
s = io.open(path, encoding='utf-8', errors='surrogateescape').read()
if old not in s:
    sys.exit(1)
io.open(path, 'w', encoding='utf-8', errors='surrogateescape').write(
    s.replace(old, new, 1))
PY
}

# case <group> <file> <old> <new> <what it is>
case_red() {
    local group=$1 file=$2 old=$3 new=$4 what=$5

    cases=$((cases + 1))

    if ! replace "$file" "$old" "$new"; then
        echo "  BROKEN  $what: $file no longer contains the text this case edits"
        wrong=$((wrong + 1))
        return
    fi

    if AMINETXDUO_SOURCE_DIR="$TREE" "$CHECKER" "$group" > "$T/out" 2>&1; then
        echo "  ESCAPED $what: $group stayed green"
        wrong=$((wrong + 1))
    fi

    cp "$ROOT/$file" "$TREE/$file"

    if ! AMINETXDUO_SOURCE_DIR="$TREE" "$CHECKER" "$group" > "$T/out" 2>&1; then
        echo "  DIRTY   $what: $group still red after putting $file back"
        sed -n 's/^  FAIL [^ ]* //p' "$T/out" | head -3
        wrong=$((wrong + 1))
    fi
}

if ! AMINETXDUO_SOURCE_DIR="$TREE" "$CHECKER" > "$T/out" 2>&1; then
    echo "argtemplates selftest: the unbroken tree does not pass"
    sed -n 's/^  FAIL [^ ]* //p' "$T/out" | head -5
    exit 1
fi


case_red enums src/tools/netstat.c \
    '"INTERFACES=-i/S,ROUTES=-r/S,ALL=-a/S,STATS=-s/S,HEALTH=-h/S"' \
    '"INTERFACES=-i/S,VERBOSE=-V/S,ROUTES=-r/S,ALL=-a/S,STATS=-s/S,HEALTH=-h/S"' \
    'a keyword inserted mid-template, enum untouched'

case_red enums src/tools/whois.c \
    '#define TEMPLATE    "QUERY/A,SERVER/K,PORT/N/K,FOLLOW/S,IPV4=-4/S,IPV6=-6/S"' \
    '#define TEMPLATE    "QUERY/A,SERVER/K,PORT/N/K,FOLLOW/S,-4=IPV4/S,-6=IPV6/S"' \
    'the -4/-6 pair respelled in one command'


case_red contract src/tools/sntp.c \
    '    args[ARG_TIMEOUT] = 0;
' '' \
    'a slot left uncleared before ReadArgs'

case_red contract src/tools/whois.c \
    'LONG p = *(LONG *)args[ARG_PORT];' \
    'LONG p = (LONG)args[ARG_PORT];' \
    'a /N slot read as the number instead of the pointer'

case_red contract src/tools/netshutdown.c \
    'args[ARG_QUIET]' '*(LONG *)args[ARG_QUIET]' \
    'a switch dereferenced'

case_red contract src/tools/tftp.c \
    '#define TEMPLATE    "HOST/A,GET/K,PUT/K,AS/K,PORT/N/K,TIMEOUT/N/K,QUIET/S," \' \
    '#define TEMPLATE    "HOST/A,GET/K,PUT=GET/K,AS/K,PORT/N/K,TIMEOUT/N/K,QUIET/S," \' \
    'the same alias on two items'

case_red contract src/tools/arp.c \
    '#define TEMPLATE    "ADDRESS,DELETE/S,SET/K,UNIT/K/N,STATS/S,QUIET/S"' \
    '#define TEMPLATE    "ADDRESS,DELETE/X,SET/K,UNIT/K/N,STATS/S,QUIET/S"' \
    'a modifier ReadArgs has no letter for'

case_red contract src/tools/shownetstatus.c \
    '#define TEMPLATE    "INTERFACE/M,INTERFACES/S,ARPCACHE=ARP/S,ROUTES/S," \' \
    '#define TEMPLATE    "INTERFACE/M,INTERFACES/M,ARPCACHE=ARP/S,ROUTES/S," \' \
    'a second /M'

case_red contract src/tools/checknetconfig.c \
    '#define TEMPLATE    "QUIET/S,VERBOSE/S"' \
    '#define TEMPLATE    "QUIET/S/A,VERBOSE/S"' \
    'a switch marked required'

case_red contract src/tools/telnet.c \
    'TOOL_VERSTAG("telnet")' 'TOOL_VERSTAG("Telnet")' \
    'the version tag and tool_name disagreeing'

case_red contract src/tools/nettrace.c \
    '    if (args[ARG_LOOPBACK] != 0 && wire)' '    if (0 && wire)' \
    'an option parsed and never read (the NetTrace LOOPBACK defect)'


case_red usage src/tools/removenetinterface.c \
    '"<interface> [FORCE] [QUIET]"' '"<interface> [FORCED] [QUIET]"' \
    'a synopsis keyword the template does not have'

case_red usage src/tools/httpd.c \
    'tool_usage("<drawer> [<port>] [-v] [TRACE]' \
    'tool_usage("<drawer> [<port>] [-x] [TRACE]' \
    'a synopsis short flag the template does not have'


case_red docs src/tools/getnetstatus.c \
    ' *     GetNetStatus CHECK/K,QUIET/S,VERSION/S' \
    ' *     GetNetStatus CHECK/K,QUIET/S' \
    'a header comment behind its template (the GetNetStatus VERSION defect)'

case_red docs src/tools/ping.c \
    ' *          -o=ONEREPLY/S,-q=QUIET/S,-s=SIZE/K/N,-t=TIMEOUT/K/N,BELL/S,HOST/A,
 *          IPV4=-4/S,IPV6=-6/S' \
    ' *          -o=ONEREPLY/S,-q=QUIET/S,-s=SIZE/K/N,-t=TIMEOUT/K/N,BELL/S,HOST/A' \
    'a header comment behind its template (the ping -4/-6 defect)'

case_red docs docs/user/AmiNetXDuo.guide \
    '      host NAME/A,TIMEOUT/N/K,IPV4=-4/S,IPV6=-6/S' \
    '      host NAME/A,TIMEOUT/N/K' \
    'the guide behind the template (the host -4/-6 defect)'

case_red docs docs/user/AmiNetXDuo.guide \
    '      INTERFACES=-i/S,ROUTES=-r/S,ALL=-a/S,STATS=-s/S,HEALTH=-h/S:' \
    '      INTERFACES=-i/S,ROUTES=-r/S,ALL=-a/S,STATS=-s/S:' \
    'one of two copies in the guide left behind (the netstat HEALTH defect)'

case_red docs src/tools/fetch.c \
    '#define TEMPLATE    "URL/A,TO/K,HEADERS/S,QUIET/S,NOVERIFY/S,TIMEOUT/N/K," \' \
    '#define TEMPLATE    "URL/A,TO=OUT/K,HEADERS/S,QUIET/S,NOVERIFY/S,TIMEOUT/N/K," \' \
    'a template renamed with the header comment left behind'

echo "argtemplates selftest: $cases cases, $wrong wrong"
[ "$wrong" -eq 0 ]
