#!/usr/bin/env bash
# BebboSSH against this stack, under FS-UAE.
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


BEB="${AMINETXDUO_BEBBOSSH_DIR:-$HOME/amiga-assets/bebbossh}"
[ -d "$BEB" ] || {
    echo "no BebboSSH binaries in $BEB, set AMINETXDUO_BEBBOSSH_DIR" >&2
    exit 2
}
BEBVER=$(cat "$BEB/VERSION" 2>/dev/null || echo unknown)

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
    echo "  with install/test/run-workbench.sh, which writes build/wb31-sys." >&2
    exit 2
}


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


SRV="$ROOT/tests/bebbossh/sshd.sh"
PORT="${AMINETXDUO_BEBBOSSH_PORT:-2223}"
if [ "$LOOPBACK" = "1" ]; then
    [ -f "$ROOT/build/bebbossh-test/id_ed25519" ] || "$SRV" start >/dev/null
else
    "$SRV" status >/dev/null 2>&1 || "$SRV" start
fi
XFER="$ROOT/build/bebbossh-test/xfer"
KEY="$ROOT/build/bebbossh-test/id_ed25519"

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


TAG="${AMINETXDUO_RUN_TAG:-bebbossh}"
STAGE="$ROOT/build/bebbossh-stage-$TAG"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs" "$STAGE/envarc/.ssh" "$STAGE/up"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$A2065" "$STAGE/devs/a2065.device"
cp "$BSD"   "$STAGE/libs/bsdsocket.library"
cp "$ADDIF" "$STAGE/AddNetInterface"

cp "$LOCALE" "$STAGE/libs/locale.library"

cp "$BEB/libcryptossh.library020" "$STAGE/libs/libcryptossh.library"
cp "$BEB/bebboscp" "$STAGE/bebboscp"
cp "$BEB/bebbossh" "$STAGE/bebbossh"

cp "$KEY" "$STAGE/id_ed25519"
cp "$XFER/tiny.bin" "$XFER/mid.bin" "$XFER/big.bin" "$STAGE/up/"

if [ "$LOOPBACK" = "1" ]; then
    cp "$BEB/bebbosshd" "$STAGE/bebbosshd"
    mkdir -p "$STAGE/envarc/ssh"

    HK="$ROOT/build/bebbossh-test/hostkey_amiga_ed25519"
    if [ ! -f "$HK" ]; then
        echo "==> host key for the in-guest server (made on the host)"
        ssh-keygen -t ed25519 -f "$HK" -N "" -q -C bebbosshd-amiga
    fi
    cp "$HK" "$STAGE/envarc/ssh/ssh_host_ed25519_key"

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

    printf '# user password
' > "$STAGE/envarc/ssh/passwd"

    cp "$KEY.pub" "$STAGE/envarc/.ssh/authorized_keys"
fi

USER_NAME="${AMINETXDUO_SSH_USER:-$(id -un)}"
HOST="${AMINETXDUO_SSH_HOST:-10.0.2.2}"

CONW1="CON:0/0/512/128/BebboSSH-A/CLOSE"
CONW2="CON:0/0/720/232/BebboSSH-B/CLOSE"
CONW3="CON:0/0/448/104/BebboSSH-R/CLOSE"

if [ "$INTERACTIVE" = "1" ]; then
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

BASEDIR="$ROOT/build/fsuae-base-$TAG"
mkdir -p "$BASEDIR/Configurations"
cat > "$BASEDIR/Configurations/Host.fs-uae" <<'EOF'
[fs-uae]
floppy_drive_volume = 0
floppy_drive_volume_empty = 0
bsdsocket_library = 0
EOF


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
    printf '%s\n%s\n' "$CONW1" "$CONW2" > "$STAGE/console.txt"
    SSH="SYS:bebbossh -v3 -i DH0:id_ed25519 -p $PORT"
    TGT="$USER_NAME@127.0.0.1"

cat > "$STAGE/commands.txt" <<EOF
SYS:AddNetInterface eth0
&SYS:bebbosshd -v6 -p $PORT
wait 10
>$SSH $TGT "Echo AMIGA-EXEC-OK"
EOF

cat > "$STAGE/stdin.txt" <<EOF
Echo "SHELL-SESSION-START"
EndCLI
EOF
    echo "<$SSH $TGT" >> "$STAGE/commands.txt"
elif [ "$INTERACTIVE" = "1" ]; then
    printf '%s\n%s\n%s\n' "$CONW1" "$CONW2" "$CONW3" > "$STAGE/console.txt"

    SSH="SYS:bebbossh -v3 -i DH0:id_ed25519 -p $PORT"
    TGT="$USER_NAME@$HOST"

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

cat > "$STAGE/stdin.txt" <<EOF
echo SHELL-SESSION-START
id -un
pwd
echo \$((6*7))
exit
EOF
    echo "<$SSH $TGT" >> "$STAGE/commands.txt"

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
    : > "$STAGE/commands.txt"
    echo "SYS:AddNetInterface eth0" >> "$STAGE/commands.txt"
    if [ "$LOOPBACK" = "1" ]; then
        echo "&SYS:bebbosshd -v3 -p $PORT" >> "$STAGE/commands.txt"
        echo "wait 10" >> "$STAGE/commands.txt"
    fi
    for c in 1 2; do
        [ "$c" = "1" ] && suf="gcm" || suf="cha"
        SCP="SYS:bebboscp -v3 --ciphers $c -i DH0:id_ed25519 -p $PORT"
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
