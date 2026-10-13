#!/usr/bin/env bash
# Build only suite helpers; shipping binaries come from the existing cross tree.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
OUT=${1:?usage: build.sh OUTPUT-DIR}
mkdir -p "$OUT"
# The toolchain path is resolved from the repository, not the caller cwd.
# shellcheck disable=SC1091
. "$ROOT/tools/amiga-toolchain.sh"
for pair in CliCrash:guest CliControl:control; do
    name=${pair%%:*}; source=${pair#*:}
    "$AMIGA_GCC" -m68000 -mregparm=0 -Os -Wall -Wextra -Werror -nostartfiles \
        -I"$AMIGA_NDK" "$ROOT/src/tools/tool_startup.S" \
        "$ROOT/tests/tools/cli/$source.c" -o "$OUT/$name"
done
