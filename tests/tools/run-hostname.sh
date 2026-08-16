#!/usr/bin/env bash
#
# THE REGRESSION TEST FOR hostname, ACROSS ITS WHOLE SURFACE.
#
#   tests/tools/run-hostname.sh [-m MODEL] [-t SECONDS] [-b BUILDDIR]
#
# WHAT THE COMMAND CLAIMS, AND HOW EACH CLAIM IS CHECKED FROM OUTSIDE IT
#
#   "reports the name AND where it came from"
#       ShowNetStatus prints `Host name: <name> (from <source>)` from the same
#       two facts, and it is a different binary reading them a different way.
#       Every claim about the name below is checked there as well as here.
#
#   "writes ENV:HOSTNAME and ENVARC:HOSTNAME"
#       tools/amiberry-run.sh's envsetup assigns ENV: to DH0:env and ENVARC: to
#       DH0:envarc, which are directories on the HOST.  So after the guest has
#       stopped, the two files are read off the host filesystem and their
#       contents asserted.  That is the strongest witness available for the
#       ENVARC: half, which nothing inside one boot can see: it is the copy
#       that survives a reboot, and a command that wrote only ENV: passes every
#       in-guest assertion and renames the machine back at the next boot.
#
#   "offers the name to the running stack, so gethostname() answers at once"
#       ShowNetStatus reads gethostname() through the library.
#
#   "a machine named by a stronger source is NOT renamed, and is told"
#       THE SECOND BOOT.  DEVS:Internet/name_resolution gets a `hostname` line,
#       which is rank 4 and the strongest of the four sources, and the same
#       `hostname <name>` that worked on the first boot must be refused with
#       WARN, must leave the running name alone, and must STILL have written
#       the files -- because that file is what names the machine if the
#       stronger source stops supplying a name.
#
#       Two boots and not one because the ranking is decided when the stack
#       reads its configuration.  Editing name_resolution under a running stack
#       changes nothing until it is restarted, so a single boot could test one
#       side of the rank or the other and not both.  Each boot is asserted to
#       have happened exactly once, so a reset still shows up.
#
# WHAT ITS TEMPLATE OFFERS, AND SO WHAT IS ALSO CHECKED
#
#   NAME        absent, to report; present, to set
#   QUIET/S     the name and nothing else, which is what a script wants
#
#   plus the refusals: a name with a character that is not allowed, a label
#   that begins or ends with a hyphen, and one too long to store.
#
# NO NETWORK IS NEEDED FOR MOST OF IT.  The first commands run before any
# interface is added, because naming a machine before its first network is the
# normal case and the one that must not need a stack.
#
# The a2065.device driver is not ours to ship: point AMINETXDUO_A2065 at one,
# or drop a copy in build/a2065.device.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

MODEL=A1200
# THE GOOD CASE IS 15 SECONDS PER BOOT, measured on playhouse3 (Amiberry 68020
# A1200 on SLIRP).  The ceiling is a boot of its own plus twice the measured
# work, the arithmetic tests/tools/run-toolleak.sh uses: an emulator that costs
# more than double what it has been seen to cost is hung, and this is what says
# so rather than something to raise.  Each boot gets its own ceiling and each
# run prints what it really took.
TIMEOUT=90
BUILD="${AMINETXDUO_BUILD:-build/cm}"

while getopts "m:t:b:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        *) sed -n '3,8p' "$0" >&2; exit 2 ;;
    esac
done

case "$BUILD" in /*) ;; *) BUILD="${BUILD#./}" ;; esac

TOOLS="$ROOT/$BUILD/src/tools"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"

for f in "$TOOLS/ToolsSmoke" "$TOOLS/AddNetInterface" "$TOOLS/hostname" \
         "$TOOLS/ShowNetStatus" "$BSD"; do
    [ -f "$f" ] || { echo "missing $f, build the tree first" >&2; exit 2; }
done

A2065="${AMINETXDUO_A2065:-}"
if [ -z "$A2065" ]; then
    for candidate in \
        "$ROOT/build/a2065.device" \
        "$HOME/amiga-assets/devs/a2065.device"
    do
        [ -f "$candidate" ] && { A2065="$candidate"; break; }
    done
fi
[ -n "$A2065" ] && [ -f "$A2065" ] || {
    echo "No a2065.device found. Set AMINETXDUO_A2065=<path>." >&2
    exit 2
}

FAILED=0
fail() { echo "FAIL: $*" >&2; FAILED=1; }
pass() { echo "  ok: $*"; }

# ------------------------------------------------------------- staging ---

# stage <dir> <name_resolution-hostname-or-empty>
stage() {
    local stage="$1" fixed="$2"

    rm -rf "$stage"
    mkdir -p "$stage/libs"
    cp -R "$ROOT/tests/netstack/devs" "$stage/devs"
    cp "$A2065" "$stage/devs/a2065.device"

    # Static and SLIRP's own numbers.  A lease is not wanted here: a DHCP
    # server that supplied option 12 would name the machine at rank 3 and every
    # assertion about ENV:HOSTNAME, which is rank 2, would be testing the
    # server instead of the command.  No ID= either, for the same reason at the
    # other end of the ranking.
    cat > "$stage/devs/NetInterfaces/eth0" <<'IFEOF'
DEVICE=a2065.device
UNIT=0
CONFIGURE=STATIC
ADDRESS=10.0.2.15
NETMASK=255.255.255.0
GATEWAY=10.0.2.2
IFEOF

    [ -z "$fixed" ] ||
        printf 'hostname %s\n' "$fixed" >> "$stage/devs/Internet/name_resolution"

    cp "$BSD"                  "$stage/libs/bsdsocket.library"
    cp "$TOOLS/AddNetInterface" "$stage/AddNetInterface"
    cp "$TOOLS/hostname"        "$stage/hostname"
    cp "$TOOLS/ShowNetStatus"   "$stage/ShowNetStatus"
}

# ------------------------------------------------------------------ run ---

# boot <stagedir> <tag>; sets REPORT, HD and ELAPSED
boot() {
    local stage="$1" tag="$2" rc started
    local extras=()

    HD="$ROOT/build/amiberry-testhd-$tag"
    started=$(date +%s)

    while IFS= read -r e; do extras+=("$e"); done \
        < <(find "$stage" -mindepth 1 -maxdepth 1)

    echo "==> booting $MODEL with the A2065 on SLIRP ($tag)"
    set +e
    AMINETXDUO_RUN_TAG="$tag" \
    "$ROOT/tools/amiberry-run.sh" -N a2065 -m "$MODEL" -t "$TIMEOUT" \
        "$TOOLS/ToolsSmoke" "${extras[@]}"
    rc=$?
    set -e
    ELAPSED=$(( $(date +%s) - started ))

    REPORT="$HD/tools.txt"
    [ -f "$REPORT" ] || {
        echo "FAIL: the guest wrote no $REPORT (run rc=$rc)" >&2; exit 1; }

    echo
    echo "================= what $tag printed ================="
    cat "$REPORT"
    echo "===================================================="
    echo

    # A TIMEOUT IS A DEFECT, NOT A RESULT.
    local wanted ran stuck
    wanted=$(grep -c . "$stage/commands.txt")
    ran=$(grep -c '^===== ' "$REPORT" || true)
    if [ "$ran" -lt "$wanted" ]; then
        stuck=$(sed -n "$((ran + 1))p" "$stage/commands.txt")
        echo "INFRA: $tag ran $ran of $wanted commands in ${ELAPSED}s against a" \
             "${TIMEOUT}s ceiling." >&2
        echo "       It stopped at: ${stuck:-<past the end of the list>}" >&2
        echo "       That command hung.  Raising -t is not the fix." >&2
        exit 2
    fi
}

block() {
    awk -v banner="$1" -v want="$2" '
        index($0, "===== " banner " =====") == 1 { n++; if (n == want) { on = 1; next } }
        on && /^----- rc / { print; exit }
        on { print }
    ' "$REPORT"
}

rc_of() { block "$1" "$2" | sed -n 's/^----- rc \([0-9-]*\),.*/\1/p'; }

want_rc() { # banner nth expected description
    local got; got=$(rc_of "$1" "$2")
    if [ "$got" = "$3" ]; then pass "$4 (rc $got)"
    else fail "$4: expected rc $3, got '${got:-nothing}'"
         block "$1" "$2" | sed 's/^/       /' >&2
    fi
}

says() { # banner nth pattern description
    if block "$1" "$2" | grep -Eq -- "$3"; then
        pass "$4"
    else
        fail "$4"
        block "$1" "$2" | sed 's/^/       /' >&2
    fi
}

# The file ENV: or ENVARC: is backed by, read off the host after the guest has
# stopped.  This is the only witness there is for the ENVARC: half.
envfile() { # env|envarc
    local f="$HD/$1/HOSTNAME"
    [ -f "$f" ] && tr -d '\r\n\0' < "$f"
}

wrote() { # env|envarc expected description
    local got; got=$(envfile "$1" || true)
    if [ "$got" = "$2" ]; then pass "$3"
    else fail "$3: $HD/$1/HOSTNAME holds '${got:-nothing}', not '$2'"; fi
}

booted_once() { # banner
    local n
    n=$(grep -c "^===== $1 =====" "$REPORT" || true)
    if [ "$n" -eq 1 ]; then
        pass "the machine booted once and ran the whole list"
    elif [ "$n" -gt 1 ]; then
        fail "THE MACHINE REBOOTED: the command list restarted"
    else
        fail "the run never reached '$1', something hung"
    fi
}

# ======================================================================= #
#  BOOT 1 -- nothing outranks ENV:HOSTNAME                                #
# ======================================================================= #

STAGE1="$ROOT/build/hostname-stage-a"
stage "$STAGE1" ""

cat > "$STAGE1/commands.txt" <<'EOF'
SYS:hostname
SYS:hostname not_a_name
SYS:hostname -leadinghyphen
SYS:hostname thisnameisfartoolongtobeahostlabelbecauseitrunspastsixtythreecharacters
SYS:hostname beast
SYS:hostname
SYS:ShowNetStatus
SYS:AddNetInterface eth0
SYS:hostname
SYS:ShowNetStatus
SYS:hostname newbeast
SYS:ShowNetStatus
SYS:hostname QUIET
EOF

boot "$STAGE1" "${AMINETXDUO_RUN_TAG:-hostname}-a"
booted_once "SYS:AddNetInterface eth0"

# ---- 1. a machine nothing has named --------------------------------------
says "SYS:hostname" 1 "This machine has no name" \
     "a machine nothing named says so, rather than inventing one"
want_rc "SYS:hostname" 1 5 "and returns WARN"

# ---- 2. the three names it will not take ---------------------------------
says "SYS:hostname not_a_name" 1 "is not a host name" \
     "an underscore is refused"
want_rc "SYS:hostname not_a_name" 1 10 "and returns ERROR"

says "SYS:hostname -leadinghyphen" 1 "is not a host name" \
     "a label starting with a hyphen is refused"
want_rc "SYS:hostname -leadinghyphen" 1 10 "and returns ERROR"

want_rc "SYS:hostname thisnameisfartoolongtobeahostlabelbecauseitrunspastsixtythreecharacters" \
        1 10 "a name too long to store is refused"

# ---- 3. setting it with no network at all --------------------------------
want_rc "SYS:hostname beast" 1 0 "a name can be set before any interface exists"
says "SYS:hostname beast" 1 "^beast" "and the name is echoed back"
says "SYS:hostname beast" 1 "named by ENV:HOSTNAME" \
     "and it says which of the four sources now names the machine"
says "SYS:hostname beast" 1 "written to ENV:HOSTNAME and ENVARC:HOSTNAME" \
     "and which files it wrote, and what outranks them at the next boot"

says "SYS:hostname" 2 "^beast" "reading it back gives the name"
says "SYS:hostname" 2 "named by ENV:HOSTNAME" "with its source"
want_rc "SYS:hostname" 2 0 "and returns OK"

# The external witness, with the stack still down: a different command reading
# the same file through ami_config_load().
says "SYS:ShowNetStatus" 1 "Host name: +beast(\.[^ ]*)? \(from ENV:HOSTNAME\)" \
     "and ShowNetStatus reads the same name and the same source off the files"

# ---- 4. and once there is a stack ----------------------------------------
says "SYS:hostname" 3 "^beast" \
     "with the network up the name is still beast"
says "SYS:hostname" 3 "named by ENV:HOSTNAME" "and still from ENV:HOSTNAME"
says "SYS:ShowNetStatus" 2 "Host name: +beast(\.[^ ]*)? \(from ENV:HOSTNAME\)" \
     "and ShowNetStatus agrees, now reading the running stack"

# ---- 5. renaming a RUNNING machine ---------------------------------------
#
# The half that needs the stack: gethostname() has to answer the new name
# without a reboot, and ShowNetStatus is what asks it.
want_rc "SYS:hostname newbeast" 1 0 "a running machine can be renamed"
says "SYS:hostname newbeast" 1 "^newbeast" "and reports the new name"
says "SYS:ShowNetStatus" 3 "Host name: +newbeast(\.[^ ]*)? \(from ENV:HOSTNAME\)" \
     "and gethostname() answers it at once, with no reboot"

# ---- 6. QUIET is the name and nothing else -------------------------------
QOUT=$(block "SYS:hostname QUIET" 1 | grep -v '^----- rc ' |
       grep -v '^[[:space:]]*$' || true)
if [ "$QOUT" = "newbeast" ]; then
    pass "QUIET prints the name and nothing else, which is what a script wants"
else
    fail "QUIET printed more than the name"
    printf '%s\n' "$QOUT" | sed 's/^/       /' >&2
fi
want_rc "SYS:hostname QUIET" 1 0 "and returns OK"

# ---- 7. the files, read off the host -------------------------------------
wrote env    newbeast "ENV:HOSTNAME holds the name that was set last"
wrote envarc newbeast "and so does ENVARC:HOSTNAME, which is what survives a reboot"

echo "==> boot 1 took ${ELAPSED}s against a ${TIMEOUT}s ceiling"
echo

# ======================================================================= #
#  BOOT 2 -- name_resolution outranks ENV:HOSTNAME                        #
# ======================================================================= #

STAGE2="$ROOT/build/hostname-stage-b"
stage "$STAGE2" "fixedname"

cat > "$STAGE2/commands.txt" <<'EOF'
SYS:AddNetInterface eth0
SYS:hostname
SYS:ShowNetStatus
SYS:hostname beast
SYS:hostname
SYS:ShowNetStatus
EOF

boot "$STAGE2" "${AMINETXDUO_RUN_TAG:-hostname}-b"
booted_once "SYS:AddNetInterface eth0"

says "SYS:hostname" 1 "^fixedname" \
     "a HOSTNAME in name_resolution names the machine"
says "SYS:hostname" 1 "named by name_resolution" \
     "and is reported as the source, which is the strongest of the four"
says "SYS:ShowNetStatus" 1 "Host name: +fixedname(\.[^ ]*)? \(from name_resolution\)" \
     "and ShowNetStatus says the same"

# THE CLAIM THIS BOOT EXISTS FOR.  The same command that worked on boot 1 must
# be refused here, because ENV:HOSTNAME is rank 2 and name_resolution is rank 4.
want_rc "SYS:hostname beast" 1 5 \
        "renaming a machine named by a stronger source returns WARN, not OK"
says "SYS:hostname beast" 1 "^fixedname" \
     "and reports the name the machine still has, not the one that was asked for"
says "SYS:hostname beast" 1 "named by name_resolution" \
     "and names the source that outranked the request"
says "SYS:hostname beast" 1 "beast is in ENV:HOSTNAME and ENVARC:HOSTNAME" \
     "and says the file was written anyway, and what that is worth"

says "SYS:hostname" 2 "^fixedname" "the machine is still fixedname afterwards"
says "SYS:ShowNetStatus" 2 "Host name: +fixedname(\.[^ ]*)? \(from name_resolution\)" \
     "and gethostname() was never given the refused name"

# The other half of the refusal: the file really was written, so the name takes
# effect if the stronger source no longer supplies one.
wrote env    beast "ENV:HOSTNAME holds the refused name"
wrote envarc beast "and so does ENVARC:HOSTNAME"

echo "==> boot 2 took ${ELAPSED}s against a ${TIMEOUT}s ceiling"
echo
if [ "$FAILED" -eq 0 ]; then
    echo "PASS: hostname, across its whole surface, on two boots"
    exit 0
fi
echo "the transcripts above are the whole run" >&2
exit 1
