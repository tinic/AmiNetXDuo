#!/usr/bin/env bash
#
# BebboSSH against this stack, under FS-UAE.
#
#   tests/bebbossh/run-bebbossh.sh [-m MODEL] [-t SECONDS] [-b BUILDDIR]
#                                  [-k MHZ] [-x] [-C FILE]
#
#   -x  take the emulator alone.  EVERY timing needs it: a transfer measured
#       while another FS-UAE shares the host is fiction, and this project has
#       already had one set of figures corrupted that way.
#   -C  a command list to stage instead of the default six transfers.
#
# WHY THIS EXISTS
#
#   BebboSSH is an independent SSH2 suite for AmigaOS -- not Dropbear, not
#   OpenSSH, not libssh -- by the author of the m68k toolchain this project
#   builds with.  It opens bsdsocket.library version 4, and it was written
#   without ever seeing our source, so it tests the ABI rather than our idea of
#   the ABI.  tools/fetch-bebbossh.sh gets it; nothing of it is vendored (it is
#   GPLv3+, this tree is MIT) and nothing of it is linked into anything here.
#
# THE 68020 CRYPTO LIBRARY IS STAGED, NOT THE DEFAULT ONE
#
#   The archive ships libcryptossh.library (68000), .library020 and .library060
#   and its ReadMe tells the user to rename the one that suits the CPU.  The
#   A1200 profile is a 68EC020, so the 020 build is staged AS
#   LIBS:libcryptossh.library.  On the author's own figures that is worth
#   roughly 2x on ChaCha20-Poly1305, so a run that staged the plain file would
#   measure the wrong binary and publish it as BebboSSH's speed.
#
# WHY THE HOST KEY IS PRE-TRUSTED
#
#   ENVARC:.ssh/known_hosts is seeded from the server's own host key before the
#   run.  BebboSSH otherwise prints "do you trust host ...? Then enter: yes"
#   and reads stdin, which is NIL: here, and the failure reads as a handshake
#   bug.  tests/bebbossh/sshd.sh knownhosts prints the line.  ENVARC: is
#   DH0:envarc, assigned by tools/envsetup.
#
# WHAT IT MEASURES
#
#   Three sizes -- 45 B, 64 KB and 256 KB -- in each direction, which is the
#   shape docs/RESEARCH.md 80 used for Dropbear so the two are comparable.  Two
#   sizes give a slope with the handshake cancelled out; three give two slopes
#   that must agree, which is what says a figure is a per-byte cost and not a
#   window or a round-trip count.
#
#   ONE END ONLY.  The far end here is an OpenSSH server on the build host, so
#   only the client's half of the crypto runs on the 68020.  docs/RESEARCH.md
#   80 put BOTH ends on the emulated CPU.  The two are not the same
#   measurement and must not be put in one column.
#
# SCORING
#
#   Read the printed verdict, not the exit status of fs-uae.  a2065.device 2.16
#   never honours AbortIO() on a pending CMD_READ, so the last CloseLibrary()
#   can hang and a run that did everything asked of it still reports a timeout.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
MODEL=A1200
TIMEOUT=1500
CLOCK=""
BUILD="${AMINETXDUO_BUILD:-build/cm}"
PERF=0
COMMANDS=""

while getopts "m:t:b:k:xC:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        k) CLOCK="$OPTARG" ;;
        x) PERF=1 ;;
        C) COMMANDS="$OPTARG" ;;
        *) echo "usage: $0 [-m model] [-t secs] [-b builddir] [-k MHz] [-x] [-C file]" >&2; exit 2 ;;
    esac
done

BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"
ADDIF="$ROOT/$BUILD/src/tools/AddNetInterface"
for f in "$BSD" "$ADDIF"; do
    [ -f "$f" ] || { echo "missing $f -- build bsdsocket_library AddNetInterface" >&2; exit 2; }
done

# ------------------------------------------------------------- BebboSSH ----

FETCH="$ROOT/tools/fetch-bebbossh.sh"
BEB=$("$FETCH" --print-dir)
"$FETCH" --check >/dev/null 2>&1 || "$FETCH" >/dev/null
BEBVER=$("$FETCH" --print-version)

# -------------------------------------------------------- locale.library ---
#
# NOT OPTIONAL, and nothing to do with this stack.  Every BebboSSH binary is
# linked against a runtime that auto-opens locale.library; without it the
# program prints "locale.library failed to load" and then WEDGES -- verified
# with no network involved at all, `bebboscp -?` on its own, on Kickstart 3.1
# and on the AROS ROM alike.  Under Enforcer it is six illegal accesses at
# NULL from inside ROM, i.e. a library base that was never checked.  It costs
# an hour to find, because the symptom is a client that connects to nothing and
# hangs, which reads exactly like a stack bug.
#
# locale.library is Commodore's and is not in Kickstart ROM, so it is located
# here and never committed -- the same arrangement as the ROM itself and
# a2065.device.  install/test/run-workbench-fsuae.sh unpacks a Workbench 3.1
# set into build/wb31-sys, which is the first place looked.
LOCALE="${AMINETXDUO_LOCALE_LIBRARY:-}"
if [ -z "$LOCALE" ]; then
    for candidate in \
        "$ROOT/build/wb31-sys/Libs/locale.library" \
        "$HOME/AmiNetXDuo/build/wb31-sys/Libs/locale.library" \
        "$HOME/amigaos/workbench/libs/locale.library" \
        "$ROOT/build/locale.library"
    do
        [ -f "$candidate" ] && { LOCALE="$candidate"; break; }
    done
fi
[ -n "$LOCALE" ] && [ -f "$LOCALE" ] || {
    echo "No locale.library found, and BebboSSH hangs without one." >&2
    echo "  Set AMINETXDUO_LOCALE_LIBRARY=<path>, or unpack a Workbench 3.1 set" >&2
    echo "  with install/test/run-workbench-fsuae.sh, which writes build/wb31-sys." >&2
    exit 2
}

# ---------------------------------------------------------------- a2065 ----

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

# --------------------------------------------------------------- server ----

SRV="$ROOT/tests/bebbossh/sshd.sh"
PORT="${AMINETXDUO_BEBBOSSH_PORT:-2223}"
"$SRV" status >/dev/null 2>&1 || "$SRV" start
XFER="$ROOT/build/bebbossh-test/xfer"
KEY="$ROOT/build/bebbossh-test/id_ed25519"

# ------------------------------------------------------------- payloads ----
#
# Deterministic, not /dev/urandom: a rerun must compare against the same bytes,
# and a payload that changes every run cannot tell a transfer bug from a
# staging bug.  AES-CTR over zeros because it is non-repeating -- a constant
# payload would flatter anything that compresses, and the payload should not be
# the reason a number looks good.
mkpayload() {
    local path="$1" size="$2"
    [ -f "$path" ] && [ "$(wc -c < "$path" | tr -d ' ')" = "$size" ] && return 0
    head -c "$size" /dev/zero \
        | openssl enc -aes-128-ctr -nosalt -k aminetxduo-payload > "$path"
}
mkdir -p "$XFER"
mkpayload "$XFER/tiny.bin"   45
mkpayload "$XFER/mid.bin"    65536
mkpayload "$XFER/big.bin"    262144

# -------------------------------------------------------------- staging ----

TAG="${AMINETXDUO_RUN_TAG:-bebbossh}"
STAGE="$ROOT/build/bebbossh-stage-$TAG"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs" "$STAGE/envarc/.ssh" "$STAGE/up"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$A2065" "$STAGE/devs/a2065.device"
cp "$BSD"   "$STAGE/libs/bsdsocket.library"
cp "$ADDIF" "$STAGE/AddNetInterface"

cp "$LOCALE" "$STAGE/libs/locale.library"

# The 020 build under the plain name -- see the header.
cp "$BEB/libcryptossh.library020" "$STAGE/libs/libcryptossh.library"
cp "$BEB/bebboscp" "$STAGE/bebboscp"
cp "$BEB/bebbossh" "$STAGE/bebbossh"

cp "$KEY" "$STAGE/id_ed25519"
cp "$XFER/tiny.bin" "$XFER/mid.bin" "$XFER/big.bin" "$STAGE/up/"

USER_NAME="${AMINETXDUO_SSH_USER:-$(id -un)}"
HOST="${AMINETXDUO_SSH_HOST:-10.0.2.2}"

# FS-UAE's own bsdsocket emulation off, as every other harness here does it, so
# a result cannot be the host-socket shim answering instead of ours.
BASEDIR="$ROOT/build/fsuae-base-$TAG"
mkdir -p "$BASEDIR/Configurations"
cat > "$BASEDIR/Configurations/Host.fs-uae" <<'EOF'
[fs-uae]
floppy_drive_volume = 0
floppy_drive_volume_empty = 0
bsdsocket_library = 0
EOF

# ---------------------------------------------------------- the commands ---

# Always, including for a -C list: a run whose host key is untrusted stops at a
# stdin prompt that NIL: never answers, and reads as a handshake failure.
"$SRV" knownhosts "$HOST" > "$STAGE/envarc/.ssh/known_hosts"
echo "==> host key pre-trusted: $(cat "$STAGE/envarc/.ssh/known_hosts")"

if [ -n "$COMMANDS" ]; then
    cp "$COMMANDS" "$STAGE/commands.txt"
    echo "==> command list: $COMMANDS"
else
    # Both AEADs, because the comparison needs both.  BebboSSH's default order
    # is aes128-gcm first, which is what a user gets; docs/RESEARCH.md 80
    # measured Dropbear on ChaCha20-Poly1305, which is what our own 68020
    # assembly implements.  Reporting one of the two would either flatter or
    # understate somebody depending on which.  --ciphers takes a preference
    # ORDER, so "1" and "2" pin one algorithm each.
    #
    # -v3 is WARN.  The default 4 (INFO) prints a line per SFTP request, which
    # over 256 KB is thousands of Write()s to a file on the host inside the
    # measured window -- that measures the logger.
    : > "$STAGE/commands.txt"
    echo "SYS:AddNetInterface eth0" >> "$STAGE/commands.txt"
    for c in 1 2; do
        [ "$c" = "1" ] && suf="gcm" || suf="cha"
        SCP="SYS:bebboscp -v3 --ciphers $c -i DH0:id_ed25519 -p $PORT"
        for sz in tiny mid big; do
            echo "$SCP $USER_NAME@$HOST:$XFER/$sz.bin DH0:dn-$suf-$sz.bin" >> "$STAGE/commands.txt"
        done
        for sz in tiny mid big; do
            echo "$SCP DH0:up/$sz.bin $USER_NAME@$HOST:$XFER/up-$suf-$sz.bin" >> "$STAGE/commands.txt"
        done
    done
fi

rm -f "$XFER"/up-*.bin

# ------------------------------------------------------------ the driver ---
#
# clients/dropbear/clientrun.c, unchanged: a list of command lines, each run
# through SystemTagList() with NP_StackSize and each timed on the GUEST's
# clock.  The stack is the reason it exists -- a Kickstart 3.1 Shell gives a
# command 4 KB, a bsdsocket call runs NetX Duo on the CALLER's stack, and
# docs/RESEARCH.md 16.9 records that too little of it is an F-line trap and a
# reboot loop the harness reports as a timeout.  Timing on the guest matters
# too: wall clock here would include emulator start-up and the boot.

. "$ROOT/tools/amiga-toolchain.sh"

RUNNER="$ROOT/build/clients/ClientRun"
mkdir -p "$ROOT/build/clients"
if [ ! -x "$RUNNER" ] || [ "$ROOT/clients/dropbear/clientrun.c" -nt "$RUNNER" ]; then
    echo "==> building ClientRun"
    "$AMIGA_GCC" -O2 -m68020 -fomit-frame-pointer -Wall -I"$AMIGA_NDK" \
                 -o "$RUNNER" "$ROOT/clients/dropbear/clientrun.c"
fi

echo "==> BebboSSH $BEBVER from $BEB"
echo "==> target: $USER_NAME@$HOST:$PORT"

export AMINETXDUO_RUN_TAG="$TAG"

CPUARG=()
[ -z "$CLOCK" ] || CPUARG+=(-k "$CLOCK")
[ "$PERF" = "1" ] && CPUARG+=(-x)

set +e
"$ROOT/tools/fsuae-run.sh" -n -m "$MODEL" -t "$TIMEOUT" ${CPUARG[@]+"${CPUARG[@]}"} \
    "$RUNNER" "$STAGE/devs" "$STAGE/libs" "$STAGE/envarc" "$STAGE/up" \
    "$STAGE/bebboscp" "$STAGE/bebbossh" \
    "$STAGE/AddNetInterface" "$STAGE/id_ed25519" "$STAGE/commands.txt"
RC=$?
set -e

HD="$ROOT/build/testhd-$TAG"
echo ""
echo "==> DH0:client.txt"
cat "$HD/client.txt" 2>/dev/null || echo "(none)"
echo ""

set +e
"$ROOT/tests/bebbossh/check.sh" "$HD" "$XFER"
VERDICT=$?
set -e

echo "fs-uae exit status was $RC; the verdict line above is the result."
exit "$VERDICT"
