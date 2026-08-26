#!/usr/bin/env bash
#
# No incomplete type may be named by a prototype in bsdsocket_vectors.h.
#
#   tools/check-vector-abi.sh [source-root]
#
# GCC drops the register ... __asm() annotations, silently and at every warning
# level, when it composes two declarations of the same function.  The fix is to
# include the header that defines the type.
#
# SPDX-License-Identifier: MIT

set -eu

ROOT="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
HDR="$ROOT/src/bsdsocket/bsdsocket_vectors.h"

[ -f "$HDR" ] || {
    echo "check-vector-abi: no $HDR" >&2
    exit 2
}

# File-scope tag declarations with no body: `struct X;`, `union X;`, and the
# opaque-typedef spelling of the same thing.
BAD=$(grep -nE '^[[:space:]]*(struct|union)[[:space:]]+[A-Za-z_][A-Za-z0-9_]*[[:space:]]*;|^[[:space:]]*typedef[[:space:]]+(struct|union)[[:space:]]+[A-Za-z_][A-Za-z0-9_]*[[:space:]]+[A-Za-z_][A-Za-z0-9_]*[[:space:]]*;' \
       "$HDR" || true)

if [ -n "$BAD" ]; then
    echo "check-vector-abi: incomplete type declared in bsdsocket_vectors.h:" >&2
    printf '%s\n' "$BAD" | sed 's/^/  /' >&2
    echo >&2
    echo "  Include the header that defines it instead.  A tag that is" >&2
    echo "  incomplete here and complete in the .c defining the vector makes" >&2
    echo "  GCC drop that vector's __asm() register annotations, with no" >&2
    echo "  diagnostic; the vector then reads its arguments off the stack." >&2
    exit 1
fi

echo "check-vector-abi: ok, every type named by a vector prototype is complete"
exit 0
