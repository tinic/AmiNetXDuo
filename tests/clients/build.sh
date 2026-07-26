#!/usr/bin/env bash
#
# Build the client access-pattern test.
#
#   tests/clients/build.sh
#
# Compiled the same way tests/conformance/conf_probe is: against the Roadshow
# NDK headers with tests/conformance/compat first on the include path, so the
# regenerated inline/bsdsocket.h wins over the NDK one (see that file for why
# the NDK's cannot be compiled by GCC 15).
#
# Output: build/clients/client_patterns
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
HERE="$ROOT/tests/clients"
COMPAT="$ROOT/tests/conformance/compat"

. "$ROOT/tools/amiga-toolchain.sh"

mkdir -p "$ROOT/build/clients"

echo "  CC client_patterns.c"
"$AMIGA_GCC" -O2 -Wall -m68020 -fomit-frame-pointer -fno-strict-aliasing \
    -I"$COMPAT" -I"$AMIGA_NDK" -include sys/types.h \
    -o "$ROOT/build/clients/client_patterns" \
    "$HERE/client_patterns.c"

echo "==> $ROOT/build/clients/client_patterns"
"$AMIGA_TOOLCHAIN_ROOT/bin/m68k-amigaos-size" \
    "$ROOT/build/clients/client_patterns" || true
