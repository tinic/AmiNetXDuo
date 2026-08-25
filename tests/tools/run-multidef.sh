#!/usr/bin/env bash
# MORE INTERFACE FILES THAN THE STACK CAN ATTACH, AND WHAT THE USER IS TOLD.
# SPDX-License-Identifier: MIT

set -uo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT" || exit 2

BUILD="${AMINETXDUO_BUILD:-build/cm}"
BOARD=a2065
TIMEOUT=240
ROUNDS="compat,3,4,8"
LIST=0

while getopts "b:t:r:N:l" opt; do
    case "$opt" in
        b) BUILD="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        r) ROUNDS="$OPTARG" ;;
        N) BOARD="$OPTARG" ;;
        l) LIST=1 ;;
        *) echo "usage: $0 [-b builddir] [-t seconds] [-r round[,round]]\
 [-N board] [-l]" >&2; exit 2 ;;
    esac
done

IFACE_PLAN="
eth0:ok
eth1:nodev
eth2:ok
eth3:badaddr
eth4:badunit
eth5:nodevline
eth6:ok
eth7:nodev
"

iface_file() { # kind
    case "$1" in
        ok)        printf 'DEVICE=a2065.device\nUNIT=0\nCONFIGURE=DHCP\n' ;;
        nodev)     printf 'DEVICE=nosuchcard.device\nUNIT=0\nCONFIGURE=DHCP\n' ;;
        badunit)   printf 'DEVICE=a2065.device\nUNIT=9\nCONFIGURE=DHCP\n' ;;
        badaddr)   printf 'DEVICE=a2065.device\nUNIT=0\nCONFIGURE=STATIC\nADDRESS=300.1.1.1\nNETMASK=255.255.255.0\n' ;;
        nodevline) printf 'UNIT=0\nCONFIGURE=DHCP\n' ;;
    esac
}

plan_for_round() { # n  -> "name:kind ..." for the first n
    printf '%s\n' "$IFACE_PLAN" | grep -v '^$' | head -n "$1"
}

kind_of() { # name -> kind
    printf '%s\n' "$IFACE_PLAN" | sed -n "s/^$1://p" | head -1
}

if [ "$LIST" = 1 ]; then
    for r in ${ROUNDS//,/ }; do
        if [ "$r" = compat ]; then
            echo "round 'compat': genet (Roadshow keywords this stack ignores)"
            echo "                badaddr (a real fault, which must still be told)"
            continue
        fi
        echo "round of $r:"
        plan_for_round "$r" | sed 's/^/    /'
    done
    exit 0
fi


BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"
ADDIF="$ROOT/$BUILD/src/tools/AddNetInterface"
SHOW="$ROOT/$BUILD/src/tools/ShowNetStatus"
CHECKCFG="$ROOT/$BUILD/src/tools/CheckNetConfig"
NETSTAT="$ROOT/$BUILD/src/tools/netstat"
SMOKE="$ROOT/$BUILD/src/tools/ToolsSmoke"
for f in "$BSD" "$ADDIF" "$SHOW" "$CHECKCFG" "$NETSTAT" "$SMOKE"; do
    [ -f "$f" ] || { echo "build $BUILD first: no $f" >&2; exit 2; }
done

[ -n "${AMINETXDUO_KICKSTART:-}" ] || {
    echo "No Kickstart.  Set AMINETXDUO_KICKSTART=<rom>." >&2; exit 2; }

A2065="${AMINETXDUO_A2065:-}"
if [ -z "$A2065" ]; then
    for c in "$ROOT/build/a2065.device" "$HOME/amiga-assets/devs/a2065.device"; do
        [ -f "$c" ] && { A2065="$c"; break; }
    done
fi
[ -n "$A2065" ] && [ -f "$A2065" ] || {
    echo "No a2065.device found.  Set AMINETXDUO_A2065=<path>." >&2; exit 2; }

RESULTS="$ROOT/build/multidef-results.txt"
: > "$RESULTS"


run_round() { # n
    local n="$1"
    local tag="matrix-multidef-$n"
    local stage="$ROOT/build/multidef-stage-$n"
    local hd="$ROOT/build/amiberry-testhd-$tag"
    local report="$hd/tools.txt"
    local names="" name kind rc bad=0

    echo
    echo "=============================================================="
    echo "==> $n interface files in DEVS:NetInterfaces/"
    echo "=============================================================="

    rm -rf "$stage"
    mkdir -p "$stage/libs" "$stage/devs/NetInterfaces"
    cp "$BSD" "$stage/libs/bsdsocket.library"
    cp "$A2065" "$stage/devs/a2065.device"

    : > "$stage/commands.txt"
    # The table BEFORE anything is attached: this is the question "what
    echo "SYS:ShowNetStatus INTERFACES" >> "$stage/commands.txt"
    echo "SYS:CheckNetConfig" >> "$stage/commands.txt"

    while IFS=: read -r name kind; do
        [ -n "$name" ] || continue
        iface_file "$kind" > "$stage/devs/NetInterfaces/$name"
        names="$names $name"
        echo "SYS:AddNetInterface $name" >> "$stage/commands.txt"
        echo "    $name ($kind)"
    done <<EOF
$(plan_for_round "$n")
EOF

    echo "SYS:ShowNetStatus INTERFACES" >> "$stage/commands.txt"

    echo "SYS:netstat -i" >> "$stage/commands.txt"

    (
        export AMINETXDUO_RUN_TAG="$tag"
        "$ROOT/tools/amiberry-run.sh" -N "$BOARD" -t "$TIMEOUT" \
            "$SMOKE" "$stage/devs" "$stage/libs" "$ADDIF" "$SHOW" \
            "$CHECKCFG" "$NETSTAT" "$stage/commands.txt"
    )
    rc=$?

    if [ ! -s "$report" ]; then
        echo "!! the guest wrote no $report (run rc=$rc)" >&2
        printf 'round=%-3s FAIL no_transcript run_rc=%s\n' "$n" "$rc" >> "$RESULTS"
        return 1
    fi

    echo
    echo "------------------ what the guest printed --------------------"
    cat "$report"
    echo "--------------------------------------------------------------"
    echo


    local table
    table=$(tr -d '\r' < "$report" |
            awk '/^===== SYS:ShowNetStatus INTERFACES =====/ { n++ }
                 n == 1 { print }
                 n > 1 { exit }' |
            sed -n '/^Interfaces$/,/^$/p' |
            grep -vE '^(Interfaces|Name[[:space:]]+State)')

    for name in $names; do
        # CLAUSE 1: visible in the table.
        if [ "$(kind_of "$name")" = nodevline ]; then
            echo "  ok   $name names no card, so no row is expected (clause 3 applies)"
        elif printf '%s\n' "$table" | grep -qE "^${name}[[:space:]]"; then
            echo "  ok   $name is visible in ShowNetStatus"
        else
            echo "  FAIL $name is DEFINED and ShowNetStatus does not list it"
            bad=$((bad + 1))
        fi

        if grep -av '^===== ' "$report" | grep -qaF -- "$name"; then
            echo "  ok   $name is named somewhere in the output"
        else
            echo "  FAIL $name is DEFINED and appears NOWHERE in any output"
            bad=$((bad + 1))
        fi
    done

    while IFS=: read -r name kind; do
        [ -n "$name" ] || continue
        [ "$kind" = ok ] && continue

        local para
        para=$(tr -d '\r' < "$report" |
               awk -v want="===== SYS:AddNetInterface $name =====" '
                   $0 == want { grab = 1; next }
                   /^===== / { grab = 0 }
                   grab { print }' |
               sed -n '/^[A-Z][A-Za-z0-9]*:[[:space:]]/,$p;/^Problems in the configuration:/,$p')

        # THE ATTACH CAP IS A REASON TOO.  Once the first valid definition has
        # attached, every later one is refused with "this stack holds 2
        # interfaces and they are all in use", whatever was wrong with the
        # FILE -- so on a round of eight most causes are never reached and the
        if printf '%s\n' "$para" |
           grep -qiE 'device|driver|unit|address|configure|memory|room|no such|in use|holds [0-9]+ interface|cannot be used|does not exist'; then
            echo "  ok   $name ($kind) is refused with a reason"
        else
            echo "  FAIL $name ($kind) is not refused with any named reason"
            bad=$((bad + 1))
        fi
    done <<EOF
$(plan_for_round "$n")
EOF

    local nsi
    nsi=$(tr -d '\r' < "$report" |
          awk '$0 == "===== SYS:netstat -i =====" { grab = 1; next }
               /^===== / { grab = 0 }
               grab { print }')

    if printf '%s\n' "$nsi" | grep -qE '^Defined but not attached:.*eth1'; then
        echo "  ok   netstat -i names eth1 as defined and not attached"
    elif printf '%s\n' "$nsi" | grep -q '^Defined but not attached:'; then
        echo "  FAIL netstat -i has the line and does not name eth1"
        printf '%s\n' "$nsi" | grep '^Defined but not attached:' | sed 's/^/       /'
        bad=$((bad + 1))
    else
        echo "  FAIL netstat -i says nothing about definitions that are not attached"
        bad=$((bad + 1))
    fi

    if grep -qai "interface files and this stack has room" "$report"; then
        echo "  note CheckNetConfig does warn about the drawer size"
    else
        echo "  note CheckNetConfig said nothing about the drawer size"
    fi

    local listed
    listed=$(printf '%s\n' "$table" | grep -cE '^[a-z0-9]+[[:space:]]' || true)
    printf 'round=%-3s defined=%-3s listed=%-3s failures=%-3s run_rc=%s\n' \
           "$n" "$n" "$listed" "$bad" "$rc" >> "$RESULTS"

    [ "$bad" = 0 ]
}

run_compat_round() {
    local tag="matrix-multidef-compat"
    local stage="$ROOT/build/multidef-stage-compat"
    local hd="$ROOT/build/amiberry-testhd-$tag"
    local report="$hd/tools.txt"
    local rc bad=0 kw

    local COMPAT_KEYWORDS="iprequests writerequests copymode multicast"

    local LECTURE='is read and does nothing|harmless and can stay|iprequests|writerequests|copymode|multicast'

    echo
    echo "=============================================================="
    echo "==> Roadshow compatibility keywords, and who is told about them"
    echo "=============================================================="

    rm -rf "$stage"
    mkdir -p "$stage/libs" "$stage/devs/NetInterfaces"
    cp "$BSD" "$stage/libs/bsdsocket.library"
    cp "$A2065" "$stage/devs/a2065.device"

    cat > "$stage/devs/NetInterfaces/genet" <<'EOF'
DEVICE=a2065.device
UNIT=0
CONFIGURE=DHCP
IPREQUESTS=32
WRITEREQUESTS=32
COPYMODE=1
MULTICAST=YES
EOF

    iface_file badaddr > "$stage/devs/NetInterfaces/badaddr"
    iface_file nodev   > "$stage/devs/NetInterfaces/wifipi"

    cat > "$stage/commands.txt" <<'EOF'
SYS:CheckNetConfig
SYS:AddNetInterface genet
SYS:ShowNetStatus INTERFACES
SYS:netstat -i
SYS:AddNetInterface badaddr
EOF

    (
        export AMINETXDUO_RUN_TAG="$tag"
        "$ROOT/tools/amiberry-run.sh" -N "$BOARD" -t "$TIMEOUT" \
            "$SMOKE" "$stage/devs" "$stage/libs" "$ADDIF" "$SHOW" \
            "$CHECKCFG" "$NETSTAT" "$stage/commands.txt"
    )
    rc=$?

    if [ ! -s "$report" ]; then
        echo "!! the guest wrote no $report (run rc=$rc)" >&2
        printf 'round=%-7s FAIL no_transcript run_rc=%s\n' compat "$rc" >> "$RESULTS"
        return 1
    fi

    echo
    echo "------------------ what the guest printed --------------------"
    cat "$report"
    echo "--------------------------------------------------------------"
    echo

    cblock() { # command-line
        tr -d '\r' < "$report" |
            awk -v want="===== $1 =====" '
                $0 == want { grab = 1; next }
                /^===== / { grab = 0 }
                grab { print }'
    }

    local cmd
    for cmd in "SYS:AddNetInterface genet" "SYS:ShowNetStatus INTERFACES" \
               "SYS:netstat -i"; do
        if cblock "$cmd" | grep -qiE "$LECTURE"; then
            echo "  FAIL '$cmd' lectures the user about keywords it ignores"
            cblock "$cmd" | grep -niE "$LECTURE" | head -4 | sed 's/^/       /'
            bad=$((bad + 1))
        else
            echo "  ok   '$cmd' says nothing about the compat keywords"
        fi
    done

    if cblock "SYS:netstat -i" | grep -q '^Network interfaces'; then
        echo "  ok   netstat -i still prints its table"
    else
        echo "  FAIL netstat -i printed no table"
        bad=$((bad + 1))
    fi

    if cblock "SYS:netstat -i" | grep -qE '^genet[[:space:]]'; then
        echo "  ok   netstat -i has a row for the interface that attached"
    else
        echo "  FAIL netstat -i has no row for genet"
        bad=$((bad + 1))
    fi

    local nlines nlecture
    nlines=$(cblock "SYS:netstat -i" | grep -c .)
    nlecture=$(cblock "SYS:netstat -i" | grep -ciE "$LECTURE")
    echo "  ..   netstat -i is $nlines lines, $nlecture of them lecture"
    if [ "$nlecture" = 0 ]; then
        echo "  ok   none of netstat -i is a lecture about ignored keywords"
    else
        echo "  FAIL $nlecture lines of netstat -i are the lecture again"
        bad=$((bad + 1))
    fi

    if cblock "SYS:netstat -i" | grep -qE '^Defined but not attached:.*wifipi'; then
        echo "  ok   netstat -i names wifipi as defined and not attached"
    else
        echo "  FAIL netstat -i does not name the definition that is not attached"
        bad=$((bad + 1))
    fi

    for kw in $COMPAT_KEYWORDS; do
        if cblock "SYS:CheckNetConfig" | grep -qi "$kw is read and does nothing"; then
            echo "  ok   CheckNetConfig still reports $kw"
        else
            echo "  FAIL CheckNetConfig no longer reports $kw"
            bad=$((bad + 1))
        fi
    done

    if cblock "SYS:CheckNetConfig" | grep -q 'Lines that are read and do nothing'; then
        echo "  ok   CheckNetConfig files them under a heading of their own"
    else
        echo "  FAIL CheckNetConfig has no heading for them"
        bad=$((bad + 1))
    fi

    if cblock "SYS:AddNetInterface badaddr" |
       grep -q 'Problems in the configuration:'; then
        echo "  ok   a real fault still reaches an ordinary command"
    else
        echo "  FAIL a real configuration fault is no longer reported"
        bad=$((bad + 1))
    fi

    if cblock "SYS:AddNetInterface badaddr" | grep -qE "ADDRESS cannot be"; then
        echo "  ok   and it names the keyword and the value"
    else
        echo "  FAIL the fault was reported without naming what is wrong"
        bad=$((bad + 1))
    fi

    if cblock "SYS:AddNetInterface badaddr" |
       grep -qE 'DEVS:NetInterfaces/badaddr, line [0-9]+'; then
        echo "  ok   and it names the file and the line"
    else
        echo "  FAIL the fault was reported without a file and a line"
        bad=$((bad + 1))
    fi

    local nblocks
    nblocks=$(cblock "SYS:AddNetInterface badaddr" |
              grep -c '^Problems in the configuration:')
    if [ "$nblocks" = 1 ]; then
        echo "  ok   and it says it exactly once"
    else
        echo "  FAIL the same fault is printed $nblocks times by one command"
        bad=$((bad + 1))
    fi

    if cblock "SYS:netstat -i" | grep -qE "ADDRESS cannot be"; then
        echo "  ok   netstat -i reports the real fault it found on the way"
    else
        echo "  FAIL netstat -i stopped reporting real configuration faults"
        bad=$((bad + 1))
    fi

    printf 'round=%-7s keywords=%-3s failures=%-3s run_rc=%s\n' \
           compat 4 "$bad" "$rc" >> "$RESULTS"

    [ "$bad" = 0 ]
}

FAILED=0
COUNT=0
for r in ${ROUNDS//,/ }; do
    COUNT=$((COUNT + 1))
    if [ "$r" = compat ]; then
        run_compat_round || FAILED=$((FAILED + 1))
    else
        run_round "$r" || FAILED=$((FAILED + 1))
    fi
done

echo
echo "=================== the definition matrix ====================="
cat "$RESULTS"
echo "==============================================================="
echo "multidef_rounds=$COUNT multidef_failed=$FAILED"

if [ "$FAILED" = 0 ]; then
    echo "multidef: PASS -- every definition is visible, and every one that"
    echo "          cannot attach is refused by name"
    exit 0
fi

echo
echo "multidef: FAIL -- $FAILED of $COUNT rounds" >&2
echo >&2
echo "  WHAT IS EXPECTED, and is not what this tree does yet:" >&2
echo >&2
echo "    definitions   UNLIMITED.  Every file in DEVS:NetInterfaces/ is" >&2
echo "                  parsed and kept.  Today the array is" >&2
echo "                  AMI_CFG_MAX_INTERFACES = 2 entries long" >&2
echo "                  (include/aminetxduo/config.h:28) and" >&2
echo "                  src/config/config_file.c:163 returns without keeping" >&2
echo "                  the rest, behind an AMI_WARN that is compiled out" >&2
echo "                  compiled out of every shipping build." >&2
echo >&2
echo "    attachment    CAPPED is fine.  Two cards is a resource limit." >&2
echo "                  Two DEFINITIONS is not." >&2
echo >&2
echo "    refusal       EXPLICIT.  ShowNetStatus must list a defined" >&2
echo "                  interface it could not attach, and say why, in the" >&2
echo "                  table a user reads to find out what they have." >&2
echo >&2
echo "  'listed' below 'defined' in the table above is the truncation." >&2
echo "  The drives are at $ROOT/build/amiberry-testhd-matrix-multidef-*." >&2
exit 1
