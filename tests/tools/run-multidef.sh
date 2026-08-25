#!/usr/bin/env bash
#
# MORE INTERFACE FILES THAN THE STACK CAN ATTACH, AND WHAT THE USER IS TOLD.
#
#   tests/tools/run-multidef.sh [-b BUILDDIR] [-t SECONDS] [-r ROUND[,ROUND]]
#                               [-N BOARD] [-l]
#
# WRITTEN RED, NOW GREEN.  This was written against the CONTRACT rather than
# against the behaviour of the day, and on 2026-08-25 the behaviour did not
# meet it: AMI_CFG_MAX_INTERFACES was 2 and a round of eight was listed as two.
# The cap was lifted the same evening (`config: a drawer may describe any
# number of interfaces...`) and this went green against it without a line of
# the assertions changing.  That order matters: an arm written at the same
# time as its fix proves nothing afterwards.  The history is kept below
# because it is what the arm is FOR.
#
# WHAT IT PROVES
#
#   Stage 3, 4 and 8 files in DEVS:NetInterfaces/, a mix of usable and
#   deliberately unusable, and ask the machine three questions:
#
#     1. Is every DEFINED interface VISIBLE to the user?  ShowNetStatus is
#        where a person looks to find out what this machine has.  A definition
#        the user wrote and the machine will not act on must still be there,
#        or the user is debugging a file the machine is pretending not to have.
#     2. Is every interface that CANNOT be attached REFUSED, by name, with the
#        reason?  Not attached, not ignored: refused, out loud.
#     3. Is anything SILENTLY DROPPED?  Every staged name must appear
#        somewhere in what the machine printed.  Silence about a file the user
#        wrote is the defect this whole harness is named for.
#
# WHY IT EXISTS
#
#   AMI_CFG_MAX_INTERFACES is 2 (include/aminetxduo/config.h:28).
#   src/config/config_file.c:163, in insert_interface(), drops everything past
#   it:
#
#       if (cfg->interface_count >= AMI_CFG_MAX_INTERFACES)
#           AMI_WARN("config: more than %ld interfaces, ignoring '%s'", ...)
#
#   AMI_WARN COMPILES OUT OF A SHIPPING BUILD.  AMINETXDUO_LOG is off in every
#   drawer that ships (tools/check-no-diag-strings.sh says why), so on a user's
#   machine that branch is a silent `return`.  A user who adds `wifi0` beside
#   `eth0` and `eth1` loses it and is told nothing, by a machine that reports
#   two interfaces as though that were all they had asked for.
#
#   AND WHICH TWO SURVIVE IS NOT ALPHABETICAL, whatever the comment above
#   insert_interface() suggests.  The array is kept sorted, but the DROP is by
#   arrival: the first two definitions the directory scan hands over are kept,
#   and only those two are then sorted.  Measured on a round of eight named
#   eth0..eth7, 2026-08-25 -- the machine kept eth2 and eth4:
#
#       Interfaces
#       Name            State    Link     Address
#       eth2            offline  ?        -
#       eth4            offline  ?        -
#
#   So it is filesystem order, which a user cannot see, cannot predict and
#   cannot change except by accident.  That is worse than alphabetical, not
#   better: an alphabetical rule can at least be explained to somebody.
#
#   NO ARM IN THIS TREE HAD EVER STAGED THREE INTERFACE FILES.  Every harness
#   under tests/ writes exactly one, or occasionally two; the cap has therefore
#   never been reached by a test, and the branch above has never executed in
#   CI.  That is why a truncation this visible survived: not because it was
#   subtle, but because nothing ever counted past two.
#
# WHAT WAS RED, precisely, and what the arm found afterwards
#
#   Clause 1 failed.  ShowNetStatus printed its Interfaces table from
#   cfg->interface_count, which was capped at 2, so a round of 8 showed two
#   names and the other six did not exist as far as the table was concerned.
#   It now lists all of them as `defined'.
#
#   AND THE ARM FOUND ONE MORE THING AFTER THE FIX LANDED, which is the whole
#   argument for keeping an arm past the defect that prompted it: on the round
#   of 8, seven of eight were listed and eth5 was not.  eth5 is the file with
#   no DEVICE line -- it names no card, so it defines no interface, and the
#   machine says exactly that by name ("the file 'eth5' cannot be used, so
#   that interface does not exist").  That is correct, so the arm exempts it
#   from clause 1 and holds it to clause 3 instead.  The finding was a defect
#   in THIS FILE's clause 1, caught by running it, and it would have shipped
#   as a permanent false red.
#
#   Clause 3 is PARTLY met, and the arm reports the difference rather than
#   flattening it.  ShowNetStatus does print a "Problems in the configuration"
#   block that names files by path, so a definition with a syntax error is
#   mentioned even when it is not tabled.  A definition that is merely
#   SURPLUS -- perfectly written, and dropped for being third -- is named
#   nowhere at all.
#
#   Clause 2 mostly passes, and it passes for a reason worth knowing: once the
#   first valid definition has attached, every later one is refused with "this
#   stack holds 2 interfaces and they are all in use" whatever was wrong with
#   the file.  So the ATTACH CAP is what a user is told, and the specific
#   fault in their file is not.  Per-cause wording is
#   tests/tools/run-bringupfail.sh's question, not this one's.
#
#   CheckNetConfig is the one command that does say something
#   (src/tools/checknetconfig.c:637-660, check_drawer_size(), which counts the
#   drawer and compares it to the cap).  It is reported here, and it is NOT
#   accepted as satisfying clause 1: it is a separate command a user has to
#   know to run, and the question "what interfaces does this machine have" is
#   ShowNetStatus's.
#
# THE CORRECT CONTRACT, so that a fix has something to aim at
#
#   definitions   UNLIMITED.  The drawer is the user's, and every file in it
#                 is a thing they said.  Parsing all of them costs a name and
#                 a few fields each.
#   attachment    CAPPED.  Two, or whatever the stack can carry; that is a
#                 real resource limit and not a parsing one.
#   refusal       EXPLICIT.  An interface that is defined and not attached is
#                 reported as defined-and-not-attached, with the reason, by
#                 the command a user runs to see their interfaces.
#
# THE `compat' ROUND, and it is the other half of the same argument
#
#   Everything above is about a machine being TOO QUIET: a file the user wrote
#   and the machine says nothing about.  `compat' is the same defect from the
#   other end, reported by the user from their own machine on 2026-08-21.
#
#   Their DEVS:NetInterfaces/genet carries four Roadshow keywords this stack
#   reads and deliberately ignores -- IPREQUESTS, WRITEREQUESTS, COPYMODE,
#   MULTICAST.  Every command that loads the configuration printed six lines
#   of prose about each of them, under the heading "Problems in the
#   configuration:", ending with the sentence "The line is harmless and can
#   stay".  `netstat -i' was thirty-three lines, twenty-one of them that
#   lecture, before the table.  The same block appeared ahead of an unrelated
#   error, so `ShowNetStatus DHCP' answered a question about one interface
#   with an essay about four keywords and then said there is no interface
#   called DHCP.
#
#   A keyword read and ignored BY DESIGN is not a problem with the file, and
#   the message said so itself.  So the round asserts a division of labour:
#
#     ordinary commands   netstat, ShowNetStatus, AddNetInterface say NOTHING
#                         about them.  Not a shorter essay: nothing.
#     CheckNetConfig      names every one of them, with its line number and
#                         its reason, because auditing the files is what that
#                         command is FOR -- and still reports the file as
#                         having nothing wrong with it, because it has not.
#     genuine faults      unchanged.  The round stages a bad ADDRESS beside
#                         the compat keywords and requires that AddNetInterface
#                         still prints it, or this arm would pass on a tree
#                         that had simply stopped reporting configuration
#                         problems altogether.
#
#   It also reads the netstat table itself: a definition that exists and is
#   not attached must be NAMED there, which is clause 1 above asked of the
#   other command a user looks at.
#
# COST: four boots, about two minutes.
#
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

# --------------------------------------------------------- the interfaces --
#
# name:kind, in the order they are written.  The KIND decides the file's
# contents and what may be said about it.  The NAME decides nothing the user
# can rely on: which definitions survive the cap is the order the directory
# scan returns them in, measured, and not the alphabet -- see the header.
#
# eth2 AND eth6 ARE DELIBERATELY VALID AND DELIBERATELY LATE.  A round whose
# only casualties were broken files would prove nothing: a machine may drop a
# file it cannot use and be forgiven.  Losing a WORKING definition, silently,
# because of where it happened to fall in a directory scan, is the finding.
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

# What each kind puts in the file, and the reason word a refusal must carry.
#
#   ok          a complete, usable definition of the card that is in the
#               machine.  Only the first of these can attach -- there is one
#               card -- so the rest are the ATTACH CAP, stated as a
#               configuration rather than as an argument.
#   nodev       DEVICE names a driver that is not on the machine
#   badunit     the right driver, a unit it does not have
#   badaddr     usable driver, an ADDRESS that is not an address
#   nodevline   no DEVICE line at all: the file does not say what card to use
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

# ------------------------------------------------------------------ rig ----

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

# ------------------------------------------------------------- one round --

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
    # interfaces does this machine have", asked of a machine that has just
    # booted, which is when a user asks it.
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

    # And again afterwards, because "defined but not attached" is a state the
    # table has to be able to show, and only the second reading can show it.
    echo "SYS:ShowNetStatus INTERFACES" >> "$stage/commands.txt"

    # AND THE OTHER COMMAND A USER LOOKS AT.  `netstat -i' reads the LIVE
    # stack, one row per attached interface, so a definition that never
    # attached appeared in no column of it at all -- a machine with four files
    # and two attached printed a two-row table and nothing to say the other
    # two exist.  ShowNetStatus grew a `defined' state for exactly this; the
    # same fact has to be reachable from here.
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

    # ---------------------------------------------------- the assertions --

    # THE FIRST READING ONLY, and this is not a detail.
    #
    # AddNetInterface takes a NAME and reads DEVS:NetInterfaces/<name>
    # directly (src/tools/addnetinterface.c:184), so it can attach a
    # definition the parsed configuration never kept -- and once attached, the
    # interface appears in the table from the live stack.  Grading the whole
    # transcript therefore reported eth2 as "visible" because the run had just
    # gone and attached it by hand, which is not the question.  The question
    # is what a user is shown when they ask a freshly booted machine what
    # interfaces it has, and only the reading BEFORE any AddNetInterface
    # answers it.
    #
    # The column header goes: it carries the word "Name", and matching it
    # would make every clause below unfailable, which is the mistake
    # tests/tools/addifup-verdict.sh was written to correct.
    local table
    table=$(tr -d '\r' < "$report" |
            awk '/^===== SYS:ShowNetStatus INTERFACES =====/ { n++ }
                 n == 1 { print }
                 n > 1 { exit }' |
            sed -n '/^Interfaces$/,/^$/p' |
            grep -vE '^(Interfaces|Name[[:space:]]+State)')

    for name in $names; do
        # CLAUSE 1: visible in the table.
        #
        # EXCEPT nodevline, and the exception is the contract rather than a
        # concession.  A file with no DEVICE line does not DEFINE an
        # interface -- it names no card, so there is nothing for the machine
        # to list a row about -- and the machine says exactly that, by name,
        # in the problems block: "the file 'eth5' cannot be used, so that
        # interface does not exist".  Demanding a table row for it would be
        # this harness insisting the machine invent an interface out of a
        # file that describes none.  Clause 3 still applies in full, and it
        # is clause 3 that carries the weight here.
        if [ "$(kind_of "$name")" = nodevline ]; then
            echo "  ok   $name names no card, so no row is expected (clause 3 applies)"
        elif printf '%s\n' "$table" | grep -qE "^${name}[[:space:]]"; then
            echo "  ok   $name is visible in ShowNetStatus"
        else
            echo "  FAIL $name is DEFINED and ShowNetStatus does not list it"
            bad=$((bad + 1))
        fi

        # CLAUSE 3: named somewhere at all.  Weaker than clause 1 on purpose:
        # a machine that refuses a definition in prose without tabling it has
        # still told the user it exists, and that is a different, smaller
        # defect than saying nothing.
        #
        # NOT OVER THE RAW TRANSCRIPT.  ToolsSmoke writes
        # `===== SYS:AddNetInterface eth3 =====` before each command
        # (src/tools/toolssmoke.c), so every staged name is in the file
        # because THIS HARNESS asked for it, and a grep over the whole thing
        # passed for every name including ones the machine never mentioned.
        # That is a clause that cannot fail, which is the thing this whole
        # directory exists to stop shipping.  The harness's own echo lines go
        # before the question is asked.
        if grep -av '^===== ' "$report" | grep -qaF -- "$name"; then
            echo "  ok   $name is named somewhere in the output"
        else
            echo "  FAIL $name is DEFINED and appears NOWHERE in any output"
            bad=$((bad + 1))
        fi
    done

    # CLAUSE 2: a refusal that names a reason.  AddNetInterface prints its
    # complaint against the name it was given, so the reason word has to be
    # found in the same paragraph rather than anywhere in the file.
    while IFS=: read -r name kind; do
        [ -n "$name" ] || continue
        [ "$kind" = ok ] && continue

        # ToolsSmoke writes `===== <command> =====` before each command
        # (src/tools/toolssmoke.c run_command()), which is the only reliable
        # boundary here: a refusal runs to several lines and the blank lines
        # inside it are part of the message, so "up to the next blank" cuts it
        # in half and reads the remainder as silence.
        # FROM THE REFUSAL ONWARD, not from the top of the command's output.
        # AddNetInterface echoes `eth1: nosuchcard.device unit 0` before it
        # tries, and that line contains the word "device" -- so a grep for a
        # reason word over the whole slice passed on the echo, for every cause,
        # whether or not a reason was ever given.  The refusal is the line
        # prefixed with the COMMAND's name, which is what tool_error() writes
        # (src/tools/tool_util.c:79); everything before it is progress.
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
        # cap is what a user is told.  That is a legitimate refusal naming a
        # legitimate reason, and grading it red here would be this harness
        # marking the stack down for answering the question it was asked.
        # Per-cause wording is tests/tools/run-bringupfail.sh's job, and it
        # orders its commands so each cause is reached.
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

    # CLAUSE 1, ASKED OF netstat.  eth1 is in every round, it parses cleanly,
    # and it names a driver that is not on this machine -- so it is DEFINED,
    # is never attached, and has no row in a table built from the live stack.
    # A user reading `netstat -i' has to be able to see that it exists.
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

    # CheckNetConfig's own count, reported and not counted.  See the header.
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

# ------------------------------------------------------ the compat round --
#
# ONE BOOT, and the whole question is WHO IS TOLD WHAT.  See the header block
# for where this came from.
#
# The drawer:
#
#   genet     the user's own file: a working a2065 definition carrying the
#             four Roadshow keywords this stack reads and ignores by design.
#             It attaches, and nothing ordinary may say a word about them.
#   badaddr   a definition with an ADDRESS that is not an address.  It is the
#             control: a real fault, which every command must STILL report.
#             Without it this round would pass on a tree that had stopped
#             reporting configuration problems at all, which is the obvious
#             wrong way to make the essay go away.
#   wifipi    a clean definition of a card this machine does not have.  Two
#             slots, three definitions: this is the one that stays DEFINED,
#             and it is what `netstat -i' has to name.  The user's machine had
#             four definitions and attached two, and the two that did not
#             appeared nowhere in that command's output.
#
run_compat_round() {
    local tag="matrix-multidef-compat"
    local stage="$ROOT/build/multidef-stage-compat"
    local hd="$ROOT/build/amiberry-testhd-$tag"
    local report="$hd/tools.txt"
    local rc bad=0 kw

    # The four the user has, spelled as their file spells them.
    local COMPAT_KEYWORDS="iprequests writerequests copymode multicast"

    # THE LECTURE, matched by its own words rather than by the heading it sat
    # under.  The heading is shared with real faults, and this round stages a
    # real fault on purpose -- grading on the heading would have required the
    # machine to stop reporting the fault too, which is the wrong fix and
    # exactly the one this arm has to be unable to accept.
    local LECTURE='is read and does nothing|harmless and can stay|iprequests|writerequests|copymode|multicast'

    echo
    echo "=============================================================="
    echo "==> Roadshow compatibility keywords, and who is told about them"
    echo "=============================================================="

    rm -rf "$stage"
    mkdir -p "$stage/libs" "$stage/devs/NetInterfaces"
    cp "$BSD" "$stage/libs/bsdsocket.library"
    cp "$A2065" "$stage/devs/a2065.device"

    # Written with the keywords apart from the working lines, and with the
    # line numbers spread out, so a report that names a line can be checked
    # against a line that really is the keyword.
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

    # Everything one command printed, between its own header and the next.
    cblock() { # command-line
        tr -d '\r' < "$report" |
            awk -v want="===== $1 =====" '
                $0 == want { grab = 1; next }
                /^===== / { grab = 0 }
                grab { print }'
    }

    # ---- the ordinary commands say NOTHING about them --------------------
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

    # AND THE TABLE IS STILL THERE, which is the other half of "nothing".  A
    # command that printed no essay because it printed nothing at all would
    # pass every assertion above.
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

    # THE DEFECT MEASURED RATHER THAN DESCRIBED.  On the user's machine
    # `netstat -i' was 33 lines, of which 21 were the lecture, before the
    # table.  The number that has to be zero is the second one; the total is
    # reported and not graded, because this drawer also holds a real fault
    # whose report is legitimately several lines long.
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

    # The definition that did not attach is named there.  Two slots, three
    # definitions: wifipi names a card this machine does not have, so it is
    # the one left over however the other two are ordered.
    if cblock "SYS:netstat -i" | grep -qE '^Defined but not attached:.*wifipi'; then
        echo "  ok   netstat -i names wifipi as defined and not attached"
    else
        echo "  FAIL netstat -i does not name the definition that is not attached"
        bad=$((bad + 1))
    fi

    # ---- CheckNetConfig still knows all of it ----------------------------
    #
    # The knowledge was not deleted, it was recategorised: the command whose
    # job is auditing these files prints every keyword, with its reason.
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

    # ---- and the genuine fault is STILL reported by an ordinary command ---
    #
    # This is the clause that stops the fix from being "print less".  badaddr
    # has an ADDRESS that is not an address; AddNetInterface must still say
    # so, with the file and the line.
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

    # ONCE.  AddNetInterface reads the file up to five times in a run, and
    # every read used to print the whole block: one bad ADDRESS came out five
    # times, identically, in the output of a single command.  Saying a true
    # thing five times is the same defect as saying a useless thing once.
    local nblocks
    nblocks=$(cblock "SYS:AddNetInterface badaddr" |
              grep -c '^Problems in the configuration:')
    if [ "$nblocks" = 1 ]; then
        echo "  ok   and it says it exactly once"
    else
        echo "  FAIL the same fault is printed $nblocks times by one command"
        bad=$((bad + 1))
    fi

    # AND BY A COMMAND THAT WAS ASKED SOMETHING ELSE.  netstat loads the same
    # drawer on its way to the table, and a fault in it is why the machine is
    # not working: it belongs on the screen of whichever command the user
    # happened to run.  That is the line this change draws -- notes never,
    # faults always -- and both halves are asserted on the same transcript.
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
