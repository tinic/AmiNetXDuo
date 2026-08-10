#!/usr/bin/env bash
#
# BebboSSH against this stack, under FS-UAE.
#
#   tests/bebbossh/run-bebbossh.sh [-m MODEL] [-t SECONDS] [-b BUILDDIR]
#                                  [-k MHZ] [-x] [-E] [-L] [-I] [-C FILE]
#
#   -x  take the emulator alone.  EVERY timing needs it: a transfer measured
#       while another FS-UAE shares the host is fiction, and this project has
#       already had one set of figures corrupted that way.
#   -C  a command list to stage instead of the default twelve transfers.
#   -I  the INTERACTIVE arm: `bebbossh` with a real AmigaDOS console, testing
#       the terminal path rather than bulk transfer.  Combines with -L, which
#       points it at bebbosshd in the guest instead of the host's sshd.
#   -L  the LOOPBACK arm: run bebbosshd in the guest as well and point bebboscp
#       at 127.0.0.1, so BOTH ends of the connection are on the one emulated
#       CPU.  That is the arrangement the Dropbear figures on the
#       `ssh-server-perf` branch used, and it is the only one comparable to
#       them.  It has to be loopback: FS-UAE's SLIRP is outbound-only, so
#       nothing on the host can open a connection INTO the guest.
#   -E  run under Enforcer + MungWall instead.  NOT A TIMING RUN: Enforcer
#       needs an MMU, so tools/enforcer-run.sh boots a 68030 and FS-UAE turns
#       cycle accounting off above a 68020.  Use it to ask whether BebboSSH
#       touches memory it should not, and take the numbers from a plain -x run.
#
# WHY THIS EXISTS
#
#   BebboSSH is an independent SSH2 suite for AmigaOS, not Dropbear, not
#   OpenSSH, not libssh, by the author of the m68k toolchain this project
#   builds with.  It opens bsdsocket.library version 4, and it was written
#   without ever seeing our source, so it tests the ABI rather than our idea of
#   the ABI.  The binaries live in the local store; nothing is vendored (it is
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
#   Three sizes, 45 B, 64 KB and 256 KB, in each direction, which is the
#   shape the Dropbear work used, so the two are comparable.  Two
#   sizes give a slope with the handshake cancelled out; three give two slopes
#   that must agree, which is what says a figure is a per-byte cost and not a
#   window or a round-trip count.
#
#   ONE END ONLY, without -L.  The far end is an OpenSSH server on the build
#   host, so only the client's half of the crypto runs on the 68020.  The
#   Dropbear figures put BOTH ends on the emulated CPU.  The two are not the
#   same measurement and must not be put in one column; -L is the one that
#   compares.
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
ENFORCE=0
LOOPBACK=0
INTERACTIVE=0

while getopts "m:t:b:k:xELIC:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        k) CLOCK="$OPTARG" ;;
        x) PERF=1 ;;
        E) ENFORCE=1 ;;
        L) LOOPBACK=1 ;;
        I) INTERACTIVE=1 ;;
        C) COMMANDS="$OPTARG" ;;
        *) echo "usage: $0 [-m model] [-t secs] [-b builddir] [-k MHz] [-x] [-E] [-L] [-I] [-C file]" >&2; exit 2 ;;
    esac
done

BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"
ADDIF="$ROOT/$BUILD/src/tools/AddNetInterface"
for f in "$BSD" "$ADDIF"; do
    [ -f "$f" ] || { echo "missing $f, build bsdsocket_library AddNetInterface" >&2; exit 2; }
done

# ------------------------------------------------------------- BebboSSH ----

# Not vendored and not downloaded: put the binaries in the local store.
BEB="${AMINETXDUO_BEBBOSSH_DIR:-$HOME/amiga-assets/bebbossh}"
[ -d "$BEB" ] || {
    echo "no BebboSSH binaries in $BEB, set AMINETXDUO_BEBBOSSH_DIR" >&2
    exit 2
}
BEBVER=$(cat "$BEB/VERSION" 2>/dev/null || echo unknown)

# -------------------------------------------------------- locale.library ---
#
# NOT OPTIONAL, and nothing to do with this stack.  Every BebboSSH binary is
# linked against a runtime that auto-opens locale.library; without it the
# program prints "locale.library failed to load" and then WEDGES, verified
# with no network involved at all, `bebboscp -?` on its own, on Kickstart 3.1
# and on the AROS ROM alike.  Under Enforcer it is six illegal accesses at
# NULL from inside ROM, i.e. a library base that was never checked.  It costs
# an hour to find, because the symptom is a client that connects to nothing and
# hangs, which reads exactly like a stack bug.
#
# locale.library is Commodore's and is not in Kickstart ROM, so it is located
# here and never committed, the same arrangement as the ROM itself and
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
# The loopback arm has its server in the guest and needs no listener here.  The
# key pair is still made by sshd.sh, since it is the same OpenSSH-format
# ed25519 key either way.
if [ "$LOOPBACK" = "1" ]; then
    [ -f "$ROOT/build/bebbossh-test/id_ed25519" ] || "$SRV" start >/dev/null
else
    "$SRV" status >/dev/null 2>&1 || "$SRV" start
fi
XFER="$ROOT/build/bebbossh-test/xfer"
KEY="$ROOT/build/bebbossh-test/id_ed25519"

# ------------------------------------------------------------- payloads ----
#
# Deterministic, not /dev/urandom: a rerun must compare against the same bytes,
# and a payload that changes every run cannot tell a transfer bug from a
# staging bug.  AES-CTR over zeros because it is non-repeating, a constant
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

# The 020 build under the plain name, see the header.
cp "$BEB/libcryptossh.library020" "$STAGE/libs/libcryptossh.library"
cp "$BEB/bebboscp" "$STAGE/bebboscp"
cp "$BEB/bebbossh" "$STAGE/bebbossh"

cp "$KEY" "$STAGE/id_ed25519"
cp "$XFER/tiny.bin" "$XFER/mid.bin" "$XFER/big.bin" "$STAGE/up/"

# ------------------------------------------------------- the loopback arm ---
#
# bebbosshd reads ENVARC:ssh/sshd_config, its host key from
# ENVARC:ssh/ssh_host_ed25519_key and the authorised keys from
# ENVARC:.ssh/authorized_keys, the client's own directory, since one machine
# is both ends here.
#
# The host key is generated on the BUILD HOST.  bebbosshkeygen would run in the
# guest in about a second, which is not the objection: the objection is that a
# host key regenerated every run cannot be pre-trusted in known_hosts, and
# BebboSSH's ReadMe describes its own random source as "rand() pimped with time
# and vpos".  ssh-keygen writes plain `openssl-key-v1`, which is exactly what
# src/loaded25519key.cpp parses.
#
# Its public key is what the client must already trust, and the fingerprint
# BebboSSH stores is base64(SHA-256(the host key blob)), OpenSSH's own
# SHA256: display with the '=' padding ssh-keygen omits.
if [ "$LOOPBACK" = "1" ]; then
    cp "$BEB/bebbosshd" "$STAGE/bebbosshd"
    mkdir -p "$STAGE/envarc/ssh"

    HK="$ROOT/build/bebbossh-test/hostkey_amiga_ed25519"
    if [ ! -f "$HK" ]; then
        echo "==> host key for the in-guest server (made on the host)"
        ssh-keygen -t ed25519 -f "$HK" -N "" -q -C bebbosshd-amiga
    fi
    cp "$HK" "$STAGE/envarc/ssh/ssh_host_ed25519_key"

    # Stack 20000 is the template's own value and is not enough here: a
    # bsdsocket call runs NetX Duo on the CALLER's stack, and 64 KB is this
    # tree's floor for any process that touches a socket.  docs/RESEARCH.md
    # 16.9 records that too little of it is an F-line trap and a reboot loop
    # the harness reports as a timeout.
    cat > "$STAGE/envarc/ssh/sshd_config" <<EOF
DebugLevel Warn
Port $PORT
ListenAddress 127.0.0.1
HostKey ENVARC:ssh/ssh_host_ed25519_key
Passwords ENVARC:ssh/passwd
SendNoop Off
Stack 65536
HomeDir RAM:
EOF

    # Present because the config names it and an unreadable file is one more
    # thing to rule out; public-key auth is what is actually used, and the
    # client cannot do passwords without a console to read one from.
    printf '# user password
' > "$STAGE/envarc/ssh/passwd"

    cp "$KEY.pub" "$STAGE/envarc/.ssh/authorized_keys"
fi

USER_NAME="${AMINETXDUO_SSH_USER:-$(id -un)}"
HOST="${AMINETXDUO_SSH_HOST:-10.0.2.2}"

# ---------------------------------------------------- the interactive arm ---
#
# THE WINDOW SIZES ARE THE MEASUREMENT.
#
# BebboSSH asks the AmigaDOS console for its size with CSI '0 q', the Window
# Bounds Report, and puts the answer in pty-req.  So the check is whether the
# remote end's `stty size` matches what the console itself says, and both
# numbers have to be collected: ClientRun prints the console's own answer, the
# remote command writes its answer into a file on the host.
#
# CON: takes PIXELS, so two windows of deliberately different and non-default
# sizes are used.  A test that only ever asked for 640x200 could not tell a
# correct implementation from one that returns a hardcoded 80x24, 640x200 in
# topaz-8 is very close to exactly that.
CONW1="CON:0/0/512/128/BebboSSH-A/CLOSE"
CONW2="CON:0/0/720/232/BebboSSH-B/CLOSE"
# The window the resize arm starts in.  ClientRun changes it to 608x176 while
# the session runs; both numbers are unlike A and B so "did it change" and
# "changed to the right thing" stay separate questions.
CONW3="CON:0/0/448/104/BebboSSH-R/CLOSE"

if [ "$INTERACTIVE" = "1" ]; then
    # The terminal BebboSSH names in pty-req is `xterm-amiga`, which no host
    # has.  Without it `tput` answers "unknown terminal type", which looks
    # like a BebboSSH defect and is not one.  Its own terminfo source ships in
    # the archive; compiling it here means the `tput` column tests what
    # BebboSSH intends rather than what a stock host happens to know.
    TI="$ROOT/build/bebbossh-test/terminfo"
    if command -v tic >/dev/null 2>&1; then
        mkdir -p "$TI"
        tic -o "$TI" "$BEB/xterm-amiga.src" >/dev/null 2>&1 \
            && echo "==> xterm-amiga terminfo compiled into $TI" \
            || echo "!! tic could not compile xterm-amiga.src; tput will be unknown-terminal" >&2
    else
        echo "!! no tic on this host; the tput column will say unknown terminal" >&2
    fi
    RES="$ROOT/build/bebbossh-test/term"
    rm -rf "$RES"; mkdir -p "$RES"
fi

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
if [ "$LOOPBACK" = "1" ]; then
    HOST=127.0.0.1
    fp=$(ssh-keygen -lf "$HK.pub" \
         | awk '{print $2}' | sed 's/^SHA256://; s/$/=/')
    printf '%s:%s ssh-ed25519 %s\n' "$HOST" "$PORT" "$fp" \
        > "$STAGE/envarc/.ssh/known_hosts"
else
    "$SRV" knownhosts "$HOST" > "$STAGE/envarc/.ssh/known_hosts"
fi
echo "==> host key pre-trusted: $(cat "$STAGE/envarc/.ssh/known_hosts")"

if [ -n "$COMMANDS" ]; then
    cp "$COMMANDS" "$STAGE/commands.txt"
    echo "==> command list: $COMMANDS"
elif [ "$INTERACTIVE" = "1" ] && [ "$LOOPBACK" = "1" ]; then
    # bebbosshd in the guest, and the client logging in to it.  There is no
    # `stty` on the other end, the other end is an AmigaDOS Shell, so this
    # arm asks a different question from the outbound one: does logging IN to
    # the Amiga give a working shell, and does the server accept the pty
    # dimensions the client sends.  -v6 on the server is what prints the
    # dimensions it received.
    printf '%s\n%s\n' "$CONW1" "$CONW2" > "$STAGE/console.txt"
    SSH="SYS:bebbossh -v3 -i DH0:id_ed25519 -p $PORT"
    TGT="$USER_NAME@127.0.0.1"

cat > "$STAGE/commands.txt" <<EOF
SYS:AddNetInterface eth0
&SYS:bebbosshd -v6 -p $PORT
wait 10
>$SSH $TGT "Echo AMIGA-EXEC-OK"
EOF

    # A shell session, piped.  Echo is a Shell built-in, so this needs nothing
    # staged in C:, which matters, because the boot drive here has only what
    # the harness put there.
    #
    # ONE command and then EndCLI, on purpose.  A four-line script executed its
    # first two and then stopped, with the server reading the remaining bytes
    # off the socket and never running them, and the client waiting for a
    # prompt that never came until the harness timed out.  The same client
    # reading the same kind of file drove a five-line session against OpenSSH
    # to a clean exit, so that is bebbosshd's shell channel and not the send
    # path, see docs/RESEARCH.md 78.11.  Kept short so this arm terminates.
cat > "$STAGE/stdin.txt" <<EOF
Echo "SHELL-SESSION-START"
EndCLI
EOF
    echo "<$SSH $TGT" >> "$STAGE/commands.txt"
elif [ "$INTERACTIVE" = "1" ]; then
    # Two console sizes, in the order ClientRun consumes them.
    printf '%s\n%s\n%s\n' "$CONW1" "$CONW2" "$CONW3" > "$STAGE/console.txt"

    SSH="SYS:bebbossh -v3 -i DH0:id_ed25519 -p $PORT"
    TGT="$USER_NAME@$HOST"

    # The probe is a script ON THE HOST taking one argument, not a semicolon
    # chain on the command line.  ClientRun's line limit is 320 characters and
    # the chain was longer than that, which does not fail, it silently
    # becomes several commands, three of which are not commands.  Everything
    # the remote learns is written on the host, because capturing it on the
    # Amiga would mean capturing the console, and the console is the thing
    # under test.
    #
    # `stty size` reads the pty's dimensions from the kernel and needs no
    # terminfo, so it is the primary evidence.  `stty -a` is the second and
    # sharper one: BebboSSH's pty-req carries a termios block setting VERASE to
    # ^H, and a host's own default is ^?, so `erase = ^H` on the remote side
    # is proof the block was parsed and applied rather than ignored.
    # The output directory and the terminfo tree are baked in rather than
    # passed, so the remote command line carries ONE absolute path instead of
    # three.  That is not tidiness: ClientRun truncates a long line, and a
    # truncated line does not fail, its tail becomes the next "command",
    # which runs, returns 10 and reads as the program refusing an argument.
    cat > "$RES/probe.sh" <<PROBE
#!/bin/sh
# Written by tests/bebbossh/run-bebbossh.sh.  \$1 names the arm.
arm="\$1"
out="$RES"
ti="$TI"
stty size              > "\$out/\$arm.stty"   2>&1
stty -a                > "\$out/\$arm.stty_a" 2>&1
tty                    > "\$out/\$arm.tty"    2>&1
printf '%s\\n' "\$TERM" > "\$out/\$arm.term"
TERMINFO="\$ti" tput cols  > "\$out/\$arm.cols"  2>&1
TERMINFO="\$ti" tput lines > "\$out/\$arm.lines" 2>&1
# TIOCGWINSZ on the terminal itself, which is what an application asks and what
# vi and less use.  fd 0 rather than 1, because tput's and this program's
# stdout is a file here, which is also why the tput columns fall back on the
# terminfo entry's static cols#80 lines#24 and say nothing about the pty.
python3 -c 'import os;s=os.get_terminal_size(0);print(s.lines,s.columns)' \\
                       > "\$out/\$arm.winsz" 2>&1
echo SESSION-OK
PROBE
    chmod +x "$RES/probe.sh"
    P="$RES/probe.sh"

: > "$STAGE/commands.txt"
cat >> "$STAGE/commands.txt" <<EOF
SYS:AddNetInterface eth0
>$SSH $TGT "$P a"
>$SSH $TGT "$P b"
$SSH -T $TGT "$P nopty"
EOF

    # A real shell session, driven from a file rather than a console.  This is
    # BebboSSH's other input path: it reads the whole file and sends it, which
    # is how a script gets piped into a session.  `exit` is the last line, so a
    # session that ends cleanly ends by itself and one that does not shows up
    # as a timeout on this command alone.
cat > "$STAGE/stdin.txt" <<EOF
echo SHELL-SESSION-START
id -un
pwd
echo \$((6*7))
exit
EOF
    echo "<$SSH $TGT" >> "$STAGE/commands.txt"

    # The resize arm.  The remote samples its own size, waits long enough for
    # ClientRun to move the window under it, and samples again.
cat > "$RES/resize.sh" <<RESIZE
#!/bin/sh
"$RES/probe.sh" r1
sleep 22
"$RES/probe.sh" r2
echo RESIZE-ARM-DONE
RESIZE
    chmod +x "$RES/resize.sh"
    echo "R$SSH $TGT \"$RES/resize.sh\"" >> "$STAGE/commands.txt"
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
    # measured window, that measures the logger.
    : > "$STAGE/commands.txt"
    echo "SYS:AddNetInterface eth0" >> "$STAGE/commands.txt"
    if [ "$LOOPBACK" = "1" ]; then
        # & starts it and does not wait; its output goes to DH0:server.txt so
        # a server logging while a client logs does not interleave two streams
        # into one file.  The wait is generous because the server has to open
        # bsdsocket.library, read a host key and bind before the first client
        # arrives, and a client that connects too early gets a refusal that
        # looks exactly like a broken listener.
        echo "&SYS:bebbosshd -v3 -p $PORT" >> "$STAGE/commands.txt"
        echo "wait 10" >> "$STAGE/commands.txt"
    fi
    for c in 1 2; do
        [ "$c" = "1" ] && suf="gcm" || suf="cha"
        SCP="SYS:bebboscp -v3 --ciphers $c -i DH0:id_ed25519 -p $PORT"
        # In the loopback arm both ends are the Amiga, so the "remote" paths
        # are Amiga paths and the results land in DH0: for the host to compare.
        if [ "$LOOPBACK" = "1" ]; then
            RSRC="DH0:up"; RDST="DH0:"
        else
            RSRC="$XFER"; RDST="$XFER/"
        fi
        for sz in tiny mid big; do
            echo "$SCP $USER_NAME@$HOST:$RSRC/$sz.bin DH0:dn-$suf-$sz.bin" >> "$STAGE/commands.txt"
        done
        for sz in tiny mid big; do
            echo "$SCP DH0:up/$sz.bin $USER_NAME@$HOST:${RDST}up-$suf-$sz.bin" >> "$STAGE/commands.txt"
        done
    done
fi

rm -f "$XFER"/up-*.bin

# ------------------------------------------------------------ the driver ---
#
# clients/dropbear/clientrun.c, unchanged: a list of command lines, each run
# through SystemTagList() with NP_StackSize and each timed on the GUEST's
# clock.  The stack is the reason it exists, a Kickstart 3.1 Shell gives a
# command 4 KB, a bsdsocket call runs NetX Duo on the CALLER's stack, and
# docs/RESEARCH.md 16.9 records that too little of it is an F-line trap and a
# reboot loop the harness reports as a timeout.  Timing on the guest matters
# too: wall clock here would include emulator start-up and the boot.

. "$ROOT/tools/amiga-toolchain.sh"

RUNNER="$ROOT/build/clients/ClientRun"
mkdir -p "$ROOT/build/clients"
if [ ! -x "$RUNNER" ] || [ "$ROOT/clients/dropbear/clientrun.c" -nt "$RUNNER" ]; then
    echo "==> building ClientRun"
    case "${CPU:-}${MODEL:-}" in
        *68000*|*A500*|*A600*|*A2000*) _guest_arch="-m68000" ;;
        *)                             _guest_arch="-m68020" ;;
    esac
    "$AMIGA_GCC" -O2 "$_guest_arch" -fomit-frame-pointer -Wall -I"$AMIGA_NDK" \
                 -o "$RUNNER" "$ROOT/clients/dropbear/clientrun.c"
fi

echo "==> BebboSSH $BEBVER from $BEB"
echo "==> target: $USER_NAME@$HOST:$PORT"

export AMINETXDUO_RUN_TAG="$TAG"

CPUARG=()
[ -z "$CLOCK" ] || CPUARG+=(-k "$CLOCK")
[ "$PERF" = "1" ] && CPUARG+=(-x)

STAGED=("$STAGE/devs" "$STAGE/libs" "$STAGE/envarc" "$STAGE/up"
        "$STAGE/bebboscp" "$STAGE/bebbossh"
        "$STAGE/AddNetInterface" "$STAGE/id_ed25519" "$STAGE/commands.txt")
[ "$LOOPBACK" = "1" ] && STAGED+=("$STAGE/bebbosshd")
[ -f "$STAGE/console.txt" ] && STAGED+=("$STAGE/console.txt")
[ -f "$STAGE/stdin.txt" ]   && STAGED+=("$STAGE/stdin.txt")

set +e
if [ "$ENFORCE" = "1" ]; then
    # -m stacks MungWall under Enforcer, which is the order mungwall.doc
    # requires.  The model argument is not passed on: enforcer-run.sh overrides
    # the CPU to a 68030 because Enforcer WEDGES on a bare 68020 rather than
    # saying it has no MMU.
    "$ROOT/tools/enforcer-run.sh" -n -m -t "$TIMEOUT" -T "$TAG" \
        "$RUNNER" "${STAGED[@]}"
else
    "$ROOT/tools/amiberry-run.sh" -N a2065 -m "$MODEL" -t "$TIMEOUT" ${CPUARG[@]+"${CPUARG[@]}"} \
        "$RUNNER" "${STAGED[@]}"
fi
RC=$?
set -e

HD="$ROOT/build/amiberry-testhd-$TAG"
echo ""
echo "==> DH0:client.txt"
cat "$HD/client.txt" 2>/dev/null || echo "(none)"
if [ "$LOOPBACK" = "1" ]; then
    echo ""
    echo "==> DH0:server.txt (bebbosshd)"
    cat "$HD/server.txt" 2>/dev/null || echo "(none)"
fi
echo ""

# In the loopback arm the "remote" end wrote into DH0: as well, so both
# directions are compared against the staged originals in one place.
set +e
if [ "$INTERACTIVE" = "1" ] && [ "$LOOPBACK" = "1" ]; then
    "$ROOT/tests/bebbossh/check-amigashell.sh" "$HD"
elif [ "$INTERACTIVE" = "1" ]; then
    "$ROOT/tests/bebbossh/check-term.sh" "$HD" "$RES"
elif [ "$LOOPBACK" = "1" ]; then
    "$ROOT/tests/bebbossh/check.sh" "$HD" "$XFER" "$HD"
else
    "$ROOT/tests/bebbossh/check.sh" "$HD" "$XFER"
fi
VERDICT=$?
set -e

echo "fs-uae exit status was $RC; the verdict line above is the result."
exit "$VERDICT"
