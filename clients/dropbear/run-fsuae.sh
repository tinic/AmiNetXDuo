#!/usr/bin/env bash
#
# Run the ported dbclient under FS-UAE, on SLIRP, against a real SSH server.
#
#   clients/dropbear/run-fsuae.sh [-m MODEL] [-t SECONDS] [-c CPU] [-k MHZ]
#                                 [-b STACKBUILD] [-D DBBUILD] [-i KEYFILE]
#                                 [-x] [-C COMMANDS] [-A]
#
#   -A  Amiberry instead of FS-UAE, which is what a headless box needs: FS-UAE
#       wants an X server and dies in GLAD before the guest boots.  Its slirp
#       is the same 10.0.2.0/24 with the host at 10.0.2.2, so the target does
#       not move.  NOT a timing lane -- -x is FS-UAE's.
#
#   -x  take the emulator alone (tools/fsuae-run.sh's measurement lane).  EVERY
#       timing here needs it: a handshake measured while two other FS-UAE
#       instances share the host is fiction, and this project has already had
#       one set of figures corrupted that way.
#   -C  a command list to stage instead of the default four connections
#   -E  a SECOND dbclient build, staged as SYS:dbclient2.
#
#       This exists because of the emulator queue, and it makes the comparison
#       better rather than merely cheaper: an A/B taken inside ONE run shares
#       the host's load, the SLIRP scheduling and the server process, so the
#       only thing that differs between the two rows is the binary.  Two runs
#       minutes apart do not have that property, and docs/RESEARCH.md 18.6
#       records what host contention does to a wire figure.
#
# WHAT IT NEEDS ON THE OTHER END
#
#   An SSH server the guest can reach.  FS-UAE's SLIRP puts the HOST at
#   10.0.2.2, so an sshd on this machine is the obvious target and is what the
#   default command list uses.  clients/dropbear/sshd-testserver.sh starts one on port
#   2222 with its own host keys, its own authorized_keys and no root:
#
#       clients/dropbear/sshd-testserver.sh start        # writes build/sshd-test/
#       clients/dropbear/run-fsuae.sh
#       clients/dropbear/sshd-testserver.sh stop
#
#   -i names the PRIVATE KEY staged onto the Amiga.  It must be in Dropbear's
#   own format, not OpenSSH's; `dropbearkey -t ed25519 -f key` writes one and
#   `dropbearkey -y -f key` prints the line for authorized_keys.
#
#   THE KEY IS GENERATED ON THE HOST ON PURPOSE, and it is not a convenience.
#   docs/RESEARCH.md's entropy note and clients/dropbear/amiga_dropbear.c both
#   say why: this machine's pool credits itself about 21 bits.  A per-session
#   ephemeral key from that pool is a risk bounded by one session; a LONG-TERM
#   private key from it is not, so no long-term key is made here.
#
# WHY -T IS IN EVERY COMMAND
#
#   -T asks for no pseudo-terminal.  AmigaOS has none -- tcgetattr() answers
#   ENOTTY in the shim, deliberately -- so `dbclient host` without -T fails at
#   "Failed to set raw TTY mode" and `dbclient -T host command` is the whole
#   supported shape.  See clients/dropbear/amiga_dropbear.c.
#
# WHY -y -y
#
#   First -y accepts an unknown host key; second -y stops it being written to
#   ~/.ssh/known_hosts.  On a directory hard drive with no home directory that
#   write would fail, and more to the point a test that trusts a key on first
#   sight should say so rather than accumulate state between runs.
#
# It uses clients/dropbear/clientrun.c, a general "run these command lines with
# a real stack" program (once shared with the curl port, now gone).  dbclient
# needs the big stack because a Kickstart 3.1 Shell gives only 4 KB.
#
# NOT A BASELINE: it depends on a server this script does not start.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
MODEL=A1200
TIMEOUT=300
CPU=""
CLOCK=""
STACK_BUILD="${AMINETXDUO_BUILD:-build/tls}"
DB_BUILD="build/dropbear"
DB_BUILD2=""
DB_SERVER=""
KEYFILE="${AMINETXDUO_DBCLIENT_KEY:-$ROOT/build/sshd-test/id_amiga}"

PERF=0
COMMANDS="${AMINETXDUO_DB_COMMANDS:-}"
# FS-UAE needs an X server and dies in GLAD on a headless box, so -A picks
# Amiberry.  Its slirp puts the host at 10.0.2.2 as well, so DBHOST is the
# same either way and the default command list needs no change.
RUNNER_KIND="${AMINETXDUO_RUNNER:-fsuae}"
BOARD=a2065
BACKEND="${AMINETXDUO_AMIBERRY_BACKEND:-slirp}"

while getopts "m:t:c:k:b:D:E:i:xC:S:AN:B:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        c) CPU="$OPTARG" ;;
        k) CLOCK="$OPTARG" ;;
        b) STACK_BUILD="$OPTARG" ;;
        D) DB_BUILD="$OPTARG" ;;
        E) DB_BUILD2="$OPTARG" ;;
        i) KEYFILE="$OPTARG" ;;
        x) PERF=1 ;;
        C) COMMANDS="$OPTARG" ;;
        S) DB_SERVER="$OPTARG" ;;
        A) RUNNER_KIND=amiberry ;;
        N) BOARD="$OPTARG" ;;
        B) BACKEND="$OPTARG" ;;
        *) echo "usage: $0 [-m model] [-t seconds] [-c cpu] [-k MHz] [-b stackbuild] [-D dbbuild] [-i key] [-x] [-C commands] [-E dbbuild2] [-S srvbuild] [-A [-N board] [-B backend]]" >&2; exit 2 ;;
    esac
done

DBCLIENT="$ROOT/$DB_BUILD/dbclient"
BSD="$ROOT/$STACK_BUILD/src/bsdsocket/bsdsocket.library"
ADDIF="$ROOT/$STACK_BUILD/src/tools/AddNetInterface"

for f in "$DBCLIENT" "$BSD" "$ADDIF"; do
    [ -f "$f" ] || { echo "missing $f -- build it first" >&2; exit 2; }
done

# ------------------------------------------------------------ the driver ---

. "$ROOT/tools/amiga-toolchain.sh"

RUNNER="$ROOT/build/clients/ClientRun"
mkdir -p "$ROOT/build/clients"
if [ ! -x "$RUNNER" ] || [ "$ROOT/clients/dropbear/clientrun.c" -nt "$RUNNER" ]; then
    echo "==> building ClientRun"
    "$AMIGA_GCC" -O2 -m68020 -fomit-frame-pointer -Wall -I"$AMIGA_NDK" \
                 -o "$RUNNER" "$ROOT/clients/dropbear/clientrun.c"
fi

# ------------------------------------------------------------- a2065 -------

A2065="${AMINETXDUO_A2065:-}"
if [ -z "$A2065" ]; then
    for candidate in \
        "$ROOT/build/a2065.device" \
        "$HOME/amiga-os-src/os-source/other_networking/sana2/bin/devs/a2065.device"
    do
        [ -f "$candidate" ] && { A2065="$candidate"; break; }
    done
fi
[ -n "$A2065" ] && [ -f "$A2065" ] || {
    echo "No a2065.device found. Set AMINETXDUO_A2065=<path>." >&2
    exit 2
}

# ------------------------------------------------ mathieeedoubbas.library --
#
# dbclient references it for the same reason curl does: this toolchain's newlib
# implements double arithmetic by calling that library, and something in the
# startup or in Dropbear's own printf reaches it.  Checked, not assumed --
# `nm build/dropbear/dbclient | grep mathieee` finds it.  It is NOT in
# Kickstart 3.1 ROM.

MATH="${AMINETXDUO_MATHIEEEDOUBBAS:-}"
if [ -z "$MATH" ]; then
    for candidate in \
        "$ROOT/build/mathieeedoubbas.library" \
        "$HOME/amiga-assets/libs/mathieeedoubbas.library" \
        "$HOME/amigaos/libs/mathieeedoubbas.library"
    do
        [ -f "$candidate" ] && { MATH="$candidate"; break; }
    done
fi

# ------------------------------------------------------------- staging ----

STAGE="$ROOT/build/dropbear-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$A2065"    "$STAGE/devs/a2065.device"
cp "$BSD"      "$STAGE/libs/bsdsocket.library"
cp "$DBCLIENT" "$STAGE/dbclient"
if [ -n "$DB_BUILD2" ]; then
    DBCLIENT2="$ROOT/$DB_BUILD2/dbclient"
    [ -f "$DBCLIENT2" ] || { echo "missing $DBCLIENT2 -- build it first" >&2; exit 2; }
    cp "$DBCLIENT2" "$STAGE/dbclient2"
    echo "==> second client staged as SYS:dbclient2: $DB_BUILD2"
fi
cp "$ADDIF"    "$STAGE/AddNetInterface"

if [ -f "$KEYFILE" ]; then
    cp "$KEYFILE" "$STAGE/id_amiga"
    echo "==> client key staged: $KEYFILE ($(wc -c < "$KEYFILE" | tr -d ' ') bytes)"

    # ...and again at the path dbclient finds ON ITS OWN, which is the one the
    # ReadMe tells a user to use and the one nothing tested until now.  The
    # shim answers getpwnam() with a home of SYS: (ENV:HOME when set), Dropbear
    # appends "/.ssh/id_dropbear", and amiga_fix_path() collapses the "SYS:/"
    # that produces.  Every other command here passes -i and therefore proves
    # none of that.
    mkdir -p "$STAGE/.ssh"
    cp "$KEYFILE" "$STAGE/.ssh/id_dropbear"
    echo "==> and as SYS:.ssh/id_dropbear, for the no -i default"
else
    echo "!! no client key at $KEYFILE -- public-key auth will fail." >&2
    echo "   clients/dropbear/sshd-testserver.sh start makes one." >&2
fi

# The ECDSA client key rides along whenever it exists.  A build made from
# clients/dropbear/localoptions-p256.h has no ed25519 at all and cannot present
# id_amiga; staging both means one run can compare algorithm families without a
# second staging directory.
ECDSAKEY=""
if [ -f "$(dirname "$KEYFILE")/id_amiga_ecdsa" ]; then
    ECDSAKEY="$(dirname "$KEYFILE")/id_amiga_ecdsa"
    cp "$ECDSAKEY" "$STAGE/id_amiga_ecdsa"
    echo "==> ECDSA client key staged as DH0:id_amiga_ecdsa"
fi

if [ -n "$MATH" ] && [ -f "$MATH" ]; then
    cp "$MATH" "$STAGE/libs/mathieeedoubbas.library"
    echo "==> mathieeedoubbas.library staged ($(wc -c < "$MATH" | tr -d ' ') bytes)"
else
    # Fatal, not a warning.  Without it every dbclient in the list exits 20 on
    # "mathieeedoubbas.library failed to load" before it opens a socket, and
    # the run took eighteen seconds and reported success having tested nothing.
    echo "!! NO mathieeedoubbas.library staged." >&2
    echo "   dbclient references it and it is not in Kickstart 3.1 ROM." >&2
    echo "   Set AMINETXDUO_MATHIEEEDOUBBAS=<path> or drop one in build/." >&2
    exit 2
fi

# ------------------------------------------------------------- the server --
#
# -S stages the Dropbear SERVER as well and points the client at 127.0.0.1
# instead of the host.
#
# It has to be loopback.  FS-UAE's SLIRP is outbound-only -- there is no way
# for anything on this machine to open a connection INTO the guest -- so a
# server on the Amiga cannot be reached from here at all.  Running both ends
# inside the guest is not a workaround for that, it is the only arrangement
# that exercises the accept() side of the stack under emulation, and it puts
# a real key exchange over the loopback interface as a bonus.
#
# The host key is generated HERE, by the host build of dropbearkey, and cached.
# Generating one in the guest is a minute of ed25519 on a 14 MHz 68020 before
# the thing being tested even starts, and it would be a different key every
# run.
#
# authorized_keys goes to DH0:.ssh/ because getpwnam() in the shim reports a
# home of SYS: and Dropbear appends "/.ssh/authorized_keys" -- which reaches
# the right place only because amiga_fix_path() collapses the "SYS:/" that
# produces.  If that ever regresses, this test is what notices.
if [ -n "$DB_SERVER" ]; then
    DBSRV="$ROOT/$DB_SERVER/dropbear"
    [ -f "$DBSRV" ] || {
        echo "missing $DBSRV -- build it with:" >&2
        echo "  clients/dropbear/build.sh -b $DB_SERVER -P \"dbclient dropbear\"" >&2
        exit 2
    }
    cp "$DBSRV" "$STAGE/dropbear"

    DBKEYGEN="$ROOT/build/dropbear-host/dropbearkey"
    [ -x "$DBKEYGEN" ] || {
        echo "missing $DBKEYGEN -- build the host tools first" >&2
        exit 2
    }

    HOSTKEY="$ROOT/build/sshd-test/hostkey_amiga_ed25519"
    if [ ! -f "$HOSTKEY" ]; then
        mkdir -p "$(dirname "$HOSTKEY")"
        echo "==> generating an Amiga host key on the host"
        "$DBKEYGEN" -t ed25519 -f "$HOSTKEY" >/dev/null
    fi
    cp "$HOSTKEY" "$STAGE/hostkey"

    mkdir -p "$STAGE/.ssh"
    "$DBKEYGEN" -y -f "$KEYFILE" \
        | sed -n 's/^Public key portion is: *//p;/^ssh-/p' > "$STAGE/.ssh/authorized_keys"
    [ -s "$STAGE/.ssh/authorized_keys" ] || {
        echo "could not extract a public key from $KEYFILE" >&2
        exit 2
    }
    echo "==> server staged; authorized_keys is $(wc -c < "$STAGE/.ssh/authorized_keys" | tr -d ' ') bytes"
fi

DBUSER="${AMINETXDUO_SSH_USER:-$(id -un)}"
DBHOST="${AMINETXDUO_SSH_HOST:-10.0.2.2}"
DBPORT="${AMINETXDUO_SSH_PORT:-2222}"

if [ -n "$COMMANDS" ]; then
    cp "$COMMANDS" "$STAGE/commands.txt"
    echo "==> command list: $COMMANDS"
elif [ -n "$DB_SERVER" ]; then
    # -F so it does not try to put itself in the background.  That is the
    # whole flag list: this build has no syslog, so it already logs to stderr
    # and there is no -E to ask for it, and DROPBEAR_SVR_PASSWORD_AUTH is 0 in
    # localoptions.h, so there is no -s to disable either.  Both were tried and
    # both are "Invalid option" -- the usage text a server prints is the list
    # of what this configuration actually built.
    #
    # authorized_keys is found through the home directory rather than -D, on
    # purpose: that path goes through amiga_fix_path() and this is what proves
    # it.
    #
    # The wait is generous on purpose: the server has to open
    # bsdsocket.library, read a host key and bind before the client arrives,
    # and this is a 14 MHz machine.  A client that connects too early gets a
    # refusal that looks exactly like a broken listener.
cat > "$STAGE/commands.txt" <<EOF
SYS:AddNetInterface eth0
&SYS:dropbear -F -r DH0:hostkey -p 2222
wait 10
SYS:dbclient -T -y -y -i DH0:id_amiga -p 2222 amiga@127.0.0.1 "echo AMIGA-SSH-SERVER-OK"
wait 5
EOF
else
cat > "$STAGE/commands.txt" <<EOF
SYS:AddNetInterface eth0
SYS:dbclient -V
SYS:dbclient -T -y -y -i DH0:id_amiga -p $DBPORT $DBUSER@$DBHOST "echo AMIGA-SSH-OK; uname -a; date"
SYS:dbclient -T -y -y -i DH0:id_amiga -p $DBPORT $DBUSER@$DBHOST "echo second connection"
# NOT a passing case: the key is staged at SYS:.ssh/id_dropbear above and
# dbclient with no -i still refuses with "No auth methods could be used".  It
# stays in the list so the ReadMe's "use -i" stops being folklore, and so that
# a future build which DOES read the default path shows up here as a change.
SYS:dbclient -T -y -y -p $DBPORT $DBUSER@$DBHOST "echo AMIGA-SSH-DEFAULTKEY-OK"
SYS:dbclient -T -y -y -i DH0:id_amiga -c aes128-ctr -p $DBPORT $DBUSER@$DBHOST "echo aes128-ctr"
SYS:dbclient -T -y -y -i DH0:id_amiga -c chacha20-poly1305@openssh.com -p $DBPORT $DBUSER@$DBHOST "echo chacha20"
EOF
fi

if [ -n "$DB_SERVER" ]; then
    echo "==> target: our own dropbear on 127.0.0.1:2222, inside the guest"
else
    echo "==> target: $DBUSER@$DBHOST:$DBPORT"
fi

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-dropbear}"

CPUARG=()
[ -z "$CPU" ]   || CPUARG+=(-c "$CPU")
[ -z "$CLOCK" ] || CPUARG+=(-k "$CLOCK")
if [ "$PERF" = "1" ]; then CPUARG+=(-x); fi

STAGED=("$STAGE/devs" "$STAGE/libs" "$STAGE/dbclient")
[ -z "$DB_BUILD2" ] || STAGED+=("$STAGE/dbclient2")
STAGED+=("$STAGE/AddNetInterface" "$STAGE/id_amiga")
[ -z "${ECDSAKEY:-}" ] || STAGED+=("$STAGE/id_amiga_ecdsa")
[ -z "$DB_SERVER" ] || STAGED+=("$STAGE/dropbear" "$STAGE/hostkey" "$STAGE/.ssh")
STAGED+=("$STAGE/commands.txt")

TRANSCRIPT="$ROOT/build/dropbear-run.log"

if [ "$RUNNER_KIND" = "amiberry" ]; then
    "$ROOT/tools/amiberry-run.sh" -N "$BOARD" -B "$BACKEND" -m "$MODEL" \
        -t "$TIMEOUT" ${CPU:+-c "$CPU"} "$RUNNER" "${STAGED[@]}" \
        2>&1 | tee "$TRANSCRIPT" || true
else
    "$ROOT/tools/fsuae-run.sh" -n -m "$MODEL" -t "$TIMEOUT" "${CPUARG[@]}" \
        "$RUNNER" "${STAGED[@]}" 2>&1 | tee "$TRANSCRIPT" || true
fi

# ------------------------------------------------------------- verdict ----
#
# The emulator exits 0 whatever the guest did, and ClientRun exits 0 having
# printed six "failed returncode 20" lines.  A run where every connection was
# refused therefore looked exactly like a passing one, which is how a missing
# disk library went unnoticed for a whole run.  So score the markers, out of
# the transcript rather than DH0:stdout.txt: the guest's own console goes to
# the serial port, and the runner prints both.
#
# Only the four commands that pass -i are pass cases.  The no-key one is in
# the list to prove the ReadMe's "use -i" is not folklore and is expected to
# fail; the version banner is not a connection at all.

[ -z "$COMMANDS" ] || { echo "==> custom command list: no verdict"; exit 0; }

echo
echo "==> verdict"
fails=0
for marker in AMIGA-SSH-OK "second connection" aes128-ctr chacha20; do
    if grep -q "^$marker" "$TRANSCRIPT"; then
        echo "  ok   $marker"
    else
        echo "  FAIL $marker"
        fails=$((fails + 1))
    fi
done

if [ "$fails" != 0 ]; then
    echo "dropbear: FAILED ($fails of 4) -- $TRANSCRIPT"
    exit 1
fi
echo "dropbear: PASSED"
