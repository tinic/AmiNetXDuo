#!/usr/bin/env bash
#
# Exercise Dropbear scp under a non-JIT Amiberry A1200/68020.  The two large
# binary copies cross the 32 KB shared-memory transport several times; the
# directory copies cover both recursive directions and timestamp handling.
#
# This uses run-dbclient.sh for the guest and sshd-testserver.sh for a private
# host-side OpenSSH instance.  Extra arguments are passed to run-dbclient.sh,
# for example:
#
#   clients/dropbear/run-scp.sh -b build/ci/default -D build/ssh \
#       -N a2065 -B slirp
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
DB_BUILD=build/dropbear
args=("$@")

# Find -D without consuming the arguments which are forwarded below.
for ((i = 0; i < ${#args[@]}; i++)); do
    if [ "${args[$i]}" = "-D" ]; then
        i=$((i + 1))
        [ "$i" -lt "${#args[@]}" ] || {
            echo "-D needs a build directory" >&2; exit 2; }
        DB_BUILD="${args[$i]}"
    fi
done

SCP="$ROOT/$DB_BUILD/scp"
SCP_RUNNER="$ROOT/$DB_BUILD/scp-runner"
[ -x "$SCP" ] || {
    echo "missing $SCP, build it with clients/dropbear/build.sh" >&2
    exit 2
}
[ -x "$SCP_RUNNER" ] || {
    echo "missing $SCP_RUNNER, build it with clients/dropbear/build.sh" >&2
    exit 2
}

TAG="${AMINETXDUO_RUN_TAG:-scp}"
PORT="${AMINETXDUO_SSH_PORT:-2222}"
USER_NAME="${AMINETXDUO_SSH_USER:-$(id -un)}"
HOST="${AMINETXDUO_SSH_HOST:-10.0.2.2}"
WORK="$ROOT/build/scp-test-$TAG"
REMOTE="$ROOT/build/sshd-test/scp-$TAG"
COMMANDS="$WORK/commands.txt"
HD="$ROOT/build/amiberry-testhd-$TAG"
RUN_LOG="$ROOT/build/scp-run-$TAG.log"

rm -rf "$WORK" "$REMOTE"
mkdir -p "$WORK/src/tree/sub/deeper" "$WORK/preexisting" \
         "$REMOTE/source-tree/sub/deeper"

# Random input makes this a byte-placement test, not merely a length test.
# 131,071 is intentionally neither a block size nor a ring multiple.
dd if=/dev/urandom of="$WORK/src/big.bin" bs=131071 count=1 status=none
cp "$WORK/src/big.bin" "$REMOTE/download.bin"
# SCP opens an existing target without O_TRUNC and truncates it after receipt.
# Starting larger makes the content comparison below cover that path.
dd if=/dev/zero of="$WORK/preexisting/download.bin" bs=200000 count=1 status=none
printf 'upload root\n' > "$WORK/src/tree/root.txt"
printf 'upload child\n' > "$WORK/src/tree/sub/child.txt"
printf 'upload deep\n' > "$WORK/src/tree/sub/deeper/deep.txt"
printf 'upload spaced name\n' > "$WORK/src/tree/sub/space name.txt"
printf 'after expected error\n' > "$WORK/src/after-error.txt"
printf 'download root\n' > "$REMOTE/source-tree/root.txt"
printf 'download child\n' > "$REMOTE/source-tree/sub/child.txt"
printf 'download deep\n' > "$REMOTE/source-tree/sub/deeper/deep.txt"
printf 'download spaced name\n' > "$REMOTE/source-tree/sub/space name.txt"
PRESERVE_MTIME=946684800
python3 -c 'import os, sys; os.utime(sys.argv[1], (int(sys.argv[2]),) * 2)' \
        "$REMOTE/source-tree/sub/deeper/deep.txt" "$PRESERVE_MTIME"

if [ -z "${AMINETXDUO_SSHD_EXTERNAL:-}" ]; then
    "$ROOT/clients/dropbear/sshd-testserver.sh" start
else
    echo "==> using the externally managed SSH server at $HOST:$PORT"
fi

cat > "$COMMANDS" <<EOF
SYS:AddNetInterface eth0
SYS:scp -S SYS:dbclient -P $PORT -o StrictHostKeyChecking=no -i DH0:id_amiga DH0:src/big.bin $USER_NAME@$HOST:$REMOTE/upload.bin
SYS:scp -S SYS:dbclient -P $PORT -o StrictHostKeyChecking=no -i DH0:id_amiga $USER_NAME@$HOST:$REMOTE/download.bin DH0:download.bin
SYS:scp -p -r -S SYS:dbclient -P $PORT -o StrictHostKeyChecking=no -i DH0:id_amiga DH0:src/tree $USER_NAME@$HOST:$REMOTE/upload-tree
SYS:scp -p -r -S SYS:dbclient -P $PORT -o StrictHostKeyChecking=no -i DH0:id_amiga $USER_NAME@$HOST:$REMOTE/source-tree DH0:download-tree
SYS:scp -S SYS:dbclient -P $PORT -o StrictHostKeyChecking=no -i DH0:id_amiga DH0:src/does-not-exist $USER_NAME@$HOST:$REMOTE/must-not-exist
SYS:scp -S SYS:dbclient -P $PORT -o StrictHostKeyChecking=no -i DH0:id_amiga DH0:src/after-error.txt $USER_NAME@$HOST:$REMOTE/after-error.txt
EOF

export AMINETXDUO_RUN_TAG="$TAG"
set +e
"$ROOT/clients/dropbear/run-dbclient.sh" "${args[@]}" \
    -C "$COMMANDS" -X "$SCP" -X "$SCP_RUNNER" -X "$WORK/src" \
    -X "$WORK/preexisting/download.bin" \
    2>&1 | tee "$RUN_LOG"
RUN_RC=${PIPESTATUS[0]}
set -e

# run-dbclient deliberately returns 77 for a custom list.  Anything else is a
# runner failure; the guest command return codes are checked below as well.
if [ "$RUN_RC" -ne 77 ]; then
    echo "scp: guest runner failed with $RUN_RC" >&2
    exit 1
fi
if ! grep -q '^run_rc=0$' "$RUN_LOG"; then
    echo "scp: emulator run did not complete cleanly" >&2
    exit 1
fi
mapfile -t COMMAND_RCS < <(sed -n 's/^--- rc \([-0-9][0-9]*\),.*/\1/p' \
                           "$HD/client.txt")
if [ "${#COMMAND_RCS[@]}" -ne 7 ] ||
   [ "${COMMAND_RCS[0]:-1}" -ne 0 ] ||
   [ "${COMMAND_RCS[1]:-1}" -ne 0 ] ||
   [ "${COMMAND_RCS[2]:-1}" -ne 0 ] ||
   [ "${COMMAND_RCS[3]:-1}" -ne 0 ] ||
   [ "${COMMAND_RCS[4]:-1}" -ne 0 ] ||
   [ "${COMMAND_RCS[5]:-0}" -eq 0 ] ||
   [ "${COMMAND_RCS[6]:-1}" -ne 0 ]; then
    echo "scp: unexpected Amiga command status sequence" >&2
    grep '^--- rc ' "$HD/client.txt" >&2 || true
    exit 1
fi

fails=0
check_file()
{
    if cmp -s "$1" "$2"; then
        echo "  ok   $3"
    else
        echo "  FAIL $3"
        fails=$((fails + 1))
    fi
}

echo "==> SCP verdict"
check_file "$WORK/src/big.bin" "$REMOTE/upload.bin" "large binary upload"
check_file "$REMOTE/download.bin" "$HD/download.bin" "large binary download and truncation"
check_file "$WORK/src/tree/root.txt" "$REMOTE/upload-tree/root.txt" "recursive upload root"
check_file "$WORK/src/tree/sub/child.txt" "$REMOTE/upload-tree/sub/child.txt" "recursive upload child"
check_file "$WORK/src/tree/sub/deeper/deep.txt" "$REMOTE/upload-tree/sub/deeper/deep.txt" "recursive upload depth"
check_file "$WORK/src/tree/sub/space name.txt" "$REMOTE/upload-tree/sub/space name.txt" "recursive upload spaced name"
check_file "$REMOTE/source-tree/root.txt" "$HD/download-tree/root.txt" "recursive download root"
check_file "$REMOTE/source-tree/sub/child.txt" "$HD/download-tree/sub/child.txt" "recursive download child"
check_file "$REMOTE/source-tree/sub/deeper/deep.txt" "$HD/download-tree/sub/deeper/deep.txt" "recursive download depth"
check_file "$REMOTE/source-tree/sub/space name.txt" "$HD/download-tree/sub/space name.txt" "recursive download spaced name"
check_file "$WORK/src/after-error.txt" "$REMOTE/after-error.txt" "recovery after expected error"

DOWNLOADED_MTIME=$(python3 -c 'import os, sys; print(int(os.stat(sys.argv[1]).st_mtime))' \
                              "$HD/download-tree/sub/deeper/deep.txt")
if [ "$DOWNLOADED_MTIME" = "$PRESERVE_MTIME" ]; then
    echo "  ok   preserved download timestamp"
else
    echo "  FAIL preserved download timestamp ($DOWNLOADED_MTIME, expected $PRESERVE_MTIME)"
    fails=$((fails + 1))
fi

if [ -e "$REMOTE/must-not-exist" ]; then
    echo "  FAIL missing-source transfer created an output"
    fails=$((fails + 1))
else
    echo "  ok   missing-source transfer failed without output"
fi

if [ "$fails" -ne 0 ]; then
    echo "scp: FAILED ($fails content mismatches)"
    exit 1
fi
echo "scp: PASSED"
