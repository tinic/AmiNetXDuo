#!/usr/bin/env bash
#
# Run the ported dbclient under FS-UAE, on SLIRP, against a real SSH server.
#
#   clients/dropbear/run-fsuae.sh [-m MODEL] [-t SECONDS] [-c CPU] [-k MHZ]
#                                 [-b STACKBUILD] [-D DBBUILD] [-i KEYFILE]
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
# Same shape as clients/curl/run-fsuae.sh, and it reuses the same driver:
# clients/curl/clientrun.c, which is a general "run these command lines with a
# real stack" program and not a curl one.  dbclient needs the big stack for
# the same reason curl does -- a Kickstart 3.1 Shell gives 4 KB.
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
KEYFILE="${AMINETXDUO_DBCLIENT_KEY:-$ROOT/build/sshd-test/id_amiga}"

while getopts "m:t:c:k:b:D:i:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        c) CPU="$OPTARG" ;;
        k) CLOCK="$OPTARG" ;;
        b) STACK_BUILD="$OPTARG" ;;
        D) DB_BUILD="$OPTARG" ;;
        i) KEYFILE="$OPTARG" ;;
        *) echo "usage: $0 [-m model] [-t seconds] [-c cpu] [-k MHz] [-b stackbuild] [-D dbbuild] [-i key]" >&2; exit 2 ;;
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
if [ ! -x "$RUNNER" ] || [ "$ROOT/clients/curl/clientrun.c" -nt "$RUNNER" ]; then
    echo "==> building ClientRun"
    "$AMIGA_GCC" -O2 -m68020 -fomit-frame-pointer -Wall -I"$AMIGA_NDK" \
                 -o "$RUNNER" "$ROOT/clients/curl/clientrun.c"
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
cp "$ADDIF"    "$STAGE/AddNetInterface"

if [ -f "$KEYFILE" ]; then
    cp "$KEYFILE" "$STAGE/id_amiga"
    echo "==> client key staged: $KEYFILE ($(wc -c < "$KEYFILE" | tr -d ' ') bytes)"
else
    echo "!! no client key at $KEYFILE -- public-key auth will fail." >&2
    echo "   clients/dropbear/sshd-testserver.sh start makes one." >&2
fi

if [ -n "$MATH" ] && [ -f "$MATH" ]; then
    cp "$MATH" "$STAGE/libs/mathieeedoubbas.library"
    echo "==> mathieeedoubbas.library staged ($(wc -c < "$MATH" | tr -d ' ') bytes)"
else
    echo "!! NO mathieeedoubbas.library staged." >&2
    echo "   dbclient references it and it is not in Kickstart 3.1 ROM." >&2
    echo "   Set AMINETXDUO_MATHIEEEDOUBBAS=<path> or drop one in build/." >&2
fi

DBUSER="${AMINETXDUO_SSH_USER:-$(id -un)}"
DBHOST="${AMINETXDUO_SSH_HOST:-10.0.2.2}"
DBPORT="${AMINETXDUO_SSH_PORT:-2222}"

if [ -n "${AMINETXDUO_DB_COMMANDS:-}" ]; then
    cp "$AMINETXDUO_DB_COMMANDS" "$STAGE/commands.txt"
    echo "==> command list: $AMINETXDUO_DB_COMMANDS"
else
cat > "$STAGE/commands.txt" <<EOF
SYS:AddNetInterface eth0
SYS:dbclient -V
SYS:dbclient -T -y -y -i DH0:id_amiga -p $DBPORT $DBUSER@$DBHOST "echo AMIGA-SSH-OK; uname -a; date"
SYS:dbclient -T -y -y -i DH0:id_amiga -p $DBPORT $DBUSER@$DBHOST "echo second connection"
SYS:dbclient -T -y -y -i DH0:id_amiga -c aes128-ctr -p $DBPORT $DBUSER@$DBHOST "echo aes128-ctr"
SYS:dbclient -T -y -y -i DH0:id_amiga -c chacha20-poly1305@openssh.com -p $DBPORT $DBUSER@$DBHOST "echo chacha20"
EOF
fi

echo "==> target: $DBUSER@$DBHOST:$DBPORT"

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-dropbear}"

CPUARG=()
[ -z "$CPU" ]   || CPUARG+=(-c "$CPU")
[ -z "$CLOCK" ] || CPUARG+=(-k "$CLOCK")

exec "$ROOT/tools/fsuae-run.sh" -n -m "$MODEL" -t "$TIMEOUT" "${CPUARG[@]}" \
     "$RUNNER" "$STAGE/devs" "$STAGE/libs" "$STAGE/dbclient" \
     "$STAGE/AddNetInterface" "$STAGE/id_amiga" "$STAGE/commands.txt"
