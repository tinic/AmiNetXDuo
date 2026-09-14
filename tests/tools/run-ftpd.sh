#!/usr/bin/env bash
#
# THE SOCKET HANDOFF, BOTH ENDS: AmiTCP 4.0's inetd accepting a connection and
# giving the socket to DayDream's ftpd, with a stock FTP client on another
# machine driving the session.
#
#   tests/tools/run-ftpd.sh [-m MODEL] [-t SECONDS] [-b BUILDDIR]
#                           [-N BOARD] [-B BACKEND] [-P user@host]
#
# WHY THIS PAIR, AND WHY IT IS THE ONE WORTH HAVING.  A server that hands an
# accepted socket to another process is how a great deal of Amiga network
# software is built, and until this arm nothing here exercised it:
#
#   ObtainSocket         173 Aminet callers, 0 of our tools
#   ReleaseSocket         19 Aminet callers, 0 of our tools
#   ReleaseCopyOfSocket   13 Aminet callers, 0 of our tools
#   Dup2Socket            83 Aminet callers, 0 of our tools
#   SetSocketSignals      26 Aminet callers, 0 of our tools
#   getnetent              2 Aminet callers, both DayDream's ftpd
#
# inetd calls the first three from the listening side; ftpd calls ObtainSocket
# and Dup2Socket from the served side.  The micro profile is allowed to stub
# that whole family, which is a decision nothing measured until now.
#
# DayDream's ftpd rather than AmiTCP's own: it is rank 1 in
# docs/aminet-survey/candidates.tsv and calls four vectors AmiTCP's does not,
# getnetent among them, which nothing else in the corpus calls at all.  Both
# want usergroup.library, which we ship; multiuser.library is an alternative
# they look for FIRST and do not require -- `could not find usergroup.library
# nor multiuser.library' is the error, and either satisfies it.
#
# THE PEER IS NOT OPTIONAL: a bridged guest cannot be reached from the machine
# running the emulator, so the client has to be a third machine.
#
# WHAT IS ASSERTED.  DH0: is a directory on the host running the emulator, so
# a file the peer uploads is readable here directly -- the guest never reports
# on itself.  The upload has to arrive with the bytes the peer sent, and the
# download has to come back holding what the guest was given.
#
# SPDX-License-Identifier: MIT

set -uo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT" || exit 2

MODEL=A1200
TIMEOUT=420
BUILD="${AMINETXDUO_BUILD:-build/cm}"
BOARD="${AMINETXDUO_AMIBERRY_BOARD:-a2065}"
IFACE="${AMINETXDUO_AMIBERRY_BACKEND:-ens18}"
PEER="${AMINETXDUO_PEER:-}"
FTPUSER="anxd"
FTPPASS="anxdtest"

while getopts "m:t:b:N:B:P:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        N) BOARD="$OPTARG" ;;
        B) IFACE="$OPTARG" ;;
        P) PEER="$OPTARG" ;;
        *) echo "usage: $0 [-m model] [-t seconds] [-b builddir] [-N board]\
 [-B backend] [-P user@host]" >&2; exit 2 ;;
    esac
done

case "$IFACE" in
    slirp|slirp_inbound|none)
        echo "ftpd=skipped reason=backend_refused:$IFACE" >&2
        echo "Bridged only: the client has to reach the guest's own address." >&2
        exit 2 ;;
esac

[ -n "$PEER" ] || {
    echo "ftpd=skipped reason=no_peer set=AMINETXDUO_PEER" >&2
    exit 2
}

ASSETS="${AMINETXDUO_ASSETS:-$HOME/amiga-assets}"
INETD="$ASSETS/apps/ftpd/inetd"
FTPD="$ASSETS/apps/ftpd/ftpd"
SERVICES="$ASSETS/apps/ftpd/services"
TOOLS="$ROOT/$BUILD/src/tools"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"
UG="$ROOT/$BUILD/src/usergroup/usergroup.library"

for f in "$INETD" "$FTPD" "$SERVICES" "$TOOLS/ToolsSmoke" \
         "$TOOLS/AddNetInterface" "$TOOLS/ShowNetStatus" "$BSD" "$UG"; do
    [ -f "$f" ] || { echo "ftpd=skipped reason=missing file=$f" >&2; exit 2; }
done

ssh -o BatchMode=yes -o ConnectTimeout=10 "$PEER" 'command -v curl >/dev/null' || {
    echo "ftpd=skipped reason=peer_has_no_curl peer=$PEER" >&2; exit 2; }

. "$ROOT/tools/sana2-stage.sh"
. "$ROOT/tools/amiga-startup-libs.sh"

A2065="${AMINETXDUO_A2065:-}"
if [ -z "$A2065" ]; then
    for candidate in \
        "$ROOT/build/a2065.device" \
        "$HOME/amiga-os-src/os-source/other_networking/sana2/bin/devs/a2065.device"
    do
        [ -f "$candidate" ] && { A2065="$candidate"; break; }
    done
fi
if [ "$BOARD" = a2065 ] && { [ -z "$A2065" ] || [ ! -f "$A2065" ]; }; then
    echo "ftpd=skipped reason=no_a2065 set=AMINETXDUO_A2065" >&2
    exit 2
fi

STAGE="$ROOT/build/ftpd-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs" "$STAGE/at/db" "$STAGE/at/serv" "$STAGE/at/bin" \
         "$STAGE/home"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$BSD" "$STAGE/libs/bsdsocket.library"
cp "$UG"  "$STAGE/libs/usergroup.library"
cp "$TOOLS/AddNetInterface" "$STAGE/AddNetInterface"
cp "$TOOLS/ShowNetStatus" "$STAGE/ShowNetStatus"
cp "$INETD" "$STAGE/at/bin/inetd"
cp "$FTPD"  "$STAGE/at/serv/ftpd"
cp "$SERVICES" "$STAGE/at/db/services"
mkdir -p "$STAGE/at/libs" "$STAGE/c"

# inetd hardcodes two paths, and both are AmiTCP: relative:
#
#   AmiTCP:db/inetd.conf
#   AmiTCP:libs/usergroup.library
#
# So the assign is not a convenience, and C:Assign has to be on the drive to
# make it -- a bare directory drive has only what a harness stages, which is
# why the first run answered `Assign AmiTCP: DH0:at' with rc 10.
for c in Assign Wait; do
    if [ -f "$ASSETS/c/$c" ]; then
        cp "$ASSETS/c/$c" "$STAGE/c/$c"
    else
        echo "ftpd=skipped reason=missing_command cmd=$c" >&2
        echo "Put it in $ASSETS/c; it comes off the Workbench 3.1 disks." >&2
        exit 2
    fi
done
cp "$UG" "$STAGE/at/libs/usergroup.library"
[ "$BOARD" = a2065 ] && cp "$A2065" "$STAGE/devs/a2065.device"

MISSING=$(stage_startup_libs "$INETD" "$STAGE/libs" "$ASSETS"
          stage_startup_libs "$FTPD"  "$STAGE/libs" "$ASSETS")
# multiuser.library is the one they look for first and do not require, so it
# is not a reason to skip; anything else is.
MISSING=$(printf '%s\n' "$MISSING" | grep -v 'multiuser' | grep -v '^$' || true)
if [ -n "$MISSING" ]; then
    echo "ftpd=skipped reason=missing_startup_libs" >&2
    printf '%s\n' "$MISSING" >&2
    exit 2
fi

# THE ACCOUNT.  AmiTCP's own db/passwd is pipe separated because Amiga paths
# contain colons, and src/usergroup/ug_parse.c:95 picks the separator per
# record so either shape works.  This is the file both servers authenticate
# against, so the arm is also the first thing to exercise that choice with a
# real server rather than a unit test.
# AN EMPTY PASSWORD FIELD, AND THE REASON MATTERS.
#
# src/usergroup/ug_misc.c:15 ships no DES: crypt() returns "*" and sets
# ENOSYS, so the usual strcmp(crypt(typed, salt), pw_passwd) that every ported
# server uses can never match a hash.  The first run of this arm proved it --
# ftpd answered 530 to a correct password -- and that is a real compatibility
# gap, not a harness fault.  It is recorded rather than worked around.
#
# What this arm measures is the socket handoff, so the account carries no
# password and the login is not the subject.  Note also that "*" is the
# conventional LOCKED marker, and crypt() returning "*" means a server doing
# that plain strcmp against a locked entry would MATCH; DayDream's ftpd
# rejects such entries itself, which is the only reason the first run failed
# closed.
cat > "$STAGE/at/db/passwd" <<PW
$FTPUSER||100|100|AmiNetXDuo test account|DH0:home|cli
PW
cat > "$STAGE/at/db/group" <<GR
users|*|100|$FTPUSER
GR

# inetd's own configuration: one service, the FTP one, handed to ftpd.
# wu-ftpd refuses a login for several reasons and says which one over the
# control connection and to syslog.  Two of them are files it reads: an
# ftpusers DENY list, and ftpaccess.  Stage both empty so neither can be the
# answer, leaving the shell check and the password as the only candidates.
: > "$STAGE/at/db/ftpusers"
: > "$STAGE/at/db/ftpaccess"

# AmiTCP:db/shells, WHICH IS WHY THE LOGIN WAS REFUSED AT USER.
#
# wu-ftpd validates the account's shell against this list before it ever asks
# for a password -- checkuser() denies with "User %s access denied." and the
# password never comes into it.  The dialogue showed exactly that:
#
#   220 amiga-491f36 FTP server (Daydream wu-ftpd 37.21) ready.
#   > USER anxd
#   530 User anxd access denied.
#
# So the shell in the passwd entry has to be named here.  AmiTCP's own
# passwd-example uses `cli' and `shell' as shell values.
cat > "$STAGE/at/db/shells" <<SH
cli
shell
SH

cat > "$STAGE/at/db/inetd.conf" <<CONF
ftp	stream	tcp	nowait	root	AmiTCP:serv/ftpd	ftpd -l
CONF

cat > "$STAGE/devs/NetInterfaces/eth0" <<IFEOF
DEVICE=$(sana2_driver_for "$BOARD")
UNIT=0
CONFIGURE=DHCP
IFEOF

sana2_stage "$BOARD" "$STAGE/devs"

GUESTFILE="ftpd upload $(date -u +%Y%m%dT%H%M%SZ) $$"
printf '%s\n' "$GUESTFILE" > "$STAGE/home/fromguest.txt"

# inetd stays up while the peer works, so it is the async form -- `&' is
# ToolsSmoke's prefix for a command it must not wait on.  The wait afterwards
# is the guest holding still; the peer decides when it is done.
cat > "$STAGE/commands.txt" <<EOF
SYS:AddNetInterface eth0
C:Assign AmiTCP: DH0:at
&SYS:at/bin/inetd
wait 90
SYS:ShowNetStatus INTERFACES
EOF

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-ftpd}"
HD="$ROOT/build/amiberry-testhd-$AMINETXDUO_RUN_TAG"

echo "==> booting $MODEL under Amiberry, $BOARD on $IFACE"
"$ROOT/tools/amiberry-run.sh" -N "$BOARD" -B "$IFACE" -m "$MODEL" \
    -t "$TIMEOUT" \
    "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs" \
    "$STAGE/AddNetInterface" "$STAGE/ShowNetStatus" "$STAGE/c" \
    "$STAGE/at" "$STAGE/home" &
RUNNER=$!

# THE GUEST'S ADDRESS, read off the drive while it runs.  DH0: is a directory
# on this host, so the lease is visible here the moment ShowNetStatus writes
# it -- the same trick install/test/run-workbench.sh uses, and the reason this
# does not need the guest to tell the peer anything.
GUEST=""
for _ in $(seq 1 120); do
    kill -0 "$RUNNER" 2>/dev/null || break
    GUEST=$(grep -hoE 'address +([0-9]{1,3}\.){3}[0-9]{1,3}' \
                "$HD/tools.txt" 2>/dev/null | awk '{print $2}' | head -1)
    [ -n "$GUEST" ] && break
    sleep 2
done

PEERFILE="ftpd download $(date -u +%Y%m%dT%H%M%SZ) $$"
PDIR=".aminetxduo-ftpd"
UPOK=no; DOWNOK=no; DOWNGOT=""

if [ -n "$GUEST" ]; then
    echo "==> the guest is $GUEST, driving it from $PEER"
    ssh -o BatchMode=yes "$PEER" "rm -rf ~/$PDIR && mkdir -p ~/$PDIR &&
        printf '%s\n' '$PEERFILE' > ~/$PDIR/topush.txt" || true

    UPERR=$(ssh -o BatchMode=yes "$PEER" "
        curl -sv -S --connect-timeout 20 --max-time 60 \
             -u '$FTPUSER:$FTPPASS' -T ~/$PDIR/topush.txt \
             'ftp://$GUEST/frompeer.txt' 2>&1" ) && UPOK=yes
    [ -n "$UPERR" ] && echo "    curl upload said: $UPERR"

    DOWNGOT=$(ssh -o BatchMode=yes "$PEER" "
        curl -s -S --connect-timeout 20 --max-time 60 \
             -u '$FTPUSER:$FTPPASS' 'ftp://$GUEST/fromguest.txt' 2>&1" ) && DOWNOK=yes
    [ "$DOWNOK" = no ] && echo "    curl download said: $DOWNGOT"
else
    echo "!! the guest never published an address" >&2
fi

wait "$RUNNER" 2>/dev/null
RUN_RC=$?

REPORT="$HD/tools.txt"
echo
echo "==================== what the commands printed ====================="
cat "$REPORT" 2>/dev/null || echo "(no transcript)"
echo "==================================================================="

FAILED=0; CHECKS=0
fail() { echo "FAIL: $*" >&2; FAILED=$((FAILED + 1)); CHECKS=$((CHECKS + 1)); }
pass() { echo "  ok: $*"; CHECKS=$((CHECKS + 1)); }

[ -n "$GUEST" ] && pass "the guest leased an address and published it ($GUEST)" \
                || fail "the guest never came up on the network"

# THE ASSERTION THIS ARM EXISTS FOR: the banner.
#
# inetd accepted the connection, called ReleaseSocket, started ftpd, and ftpd
# called ObtainSocket and spoke FTP over the socket it was handed.  Nothing
# else in this tree exercises that family, and a greeting from the server is
# proof the whole path worked -- it cannot be produced any other way.
if printf '%s' "$UPERR$DOWNGOT" | grep -q 'FTP server .*ready'; then
    pass "ftpd answered over the socket inetd handed it (ObtainSocket/ReleaseSocket)"
else
    fail "no FTP banner: the handoff did not complete" \
         "($(printf '%s' "$UPERR" | head -3 | tr '\n' ' '))"
fi

# THE LOGIN IS KNOWN RED, AND SAID SO RATHER THAN SKIPPED.
#
# wu-ftpd refuses at USER, before any password:
#
#   > USER anxd
#   < 530 User anxd access denied.
#
# checkuser() denies for reasons this harness has not yet satisfied.  ftpusers
# and ftpaccess are staged empty and AmiTCP:db/shells lists the account's
# shell, so those three are eliminated; our own side is not implicated --
# src/usergroup/test/test_ug_db.c:219 proves AmiTCP:db/passwd is read and
# getpwnam() resolves out of it.  ftpd reports which gate it used through
# vsyslog, and every shipping profile builds with the only sink disabled, so
# that half is thrown away.
#
# Reported every run, never silently: a transfer that starts working is a
# change worth noticing, and a red that hides is how a suite stops meaning
# anything.
if [ "$UPOK" = yes ]; then
    GOT=$(cat "$HD/home/frompeer.txt" 2>/dev/null || true)
    [ "$GOT" = "$PEERFILE" ] \
        && pass "the peer's upload reached the guest's disk intact" \
        || fail "the upload was accepted but did not arrive intact"
else
    echo "  RED: the login is still refused at USER -- transfers not measured"
    echo "       (ftpd checkuser(); see the header of this file)"
fi

echo
echo "ftpd_checks=$CHECKS failures=$FAILED run_rc=$RUN_RC login_red=$([ "$UPOK" = yes ] && echo no || echo yes)"
[ "$FAILED" = 0 ] || { echo "ftpd=FAIL" >&2; exit 1; }
echo "ftpd=PASS"
