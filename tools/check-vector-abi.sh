#!/usr/bin/env bash
#
# No incomplete type may be named by a prototype in bsdsocket_vectors.h.
#
#   tools/check-vector-abi.sh [source-root]
#
# Every vector in that header passes its arguments in registers, and says so
# with register ... __asm("a0") on each parameter.  GCC drops those annotations,
# silently and at every warning level, when it has to COMPOSE two declarations
# of the same function: if a struct tag is incomplete where the prototype is
# parsed and complete where the definition is, the definition's type is not the
# prototype's type, GCC forms the composite, and the register names are not part
# of it.  The vector then takes its arguments off the stack while the caller
# still puts them in registers.
#
#   struct if_nameindex;                                  /* the trigger */
#   VOID bsd_if_freenameindex(register struct if_nameindex *ptr __asm("a0"),
#                             register struct AmiSocketBase *b  __asm("a6"));
#
# That was if_freenameindex() until 2026-08-10.  It read `ptr` off the stack,
# so it freed whatever the caller happened to have at sp+4: usually zero, which
# leaks the block, and otherwise a FreeVec() of an arbitrary address, which
# takes the machine down inside exec's memory list.  The return type counts
# too -- if_nameindex() takes no pointer argument and lost its a6 the same way.
#
# The fix is to include the header that defines the type.  This is the alarm
# for the next forward declaration, because nothing else is: no diagnostic, no
# link error, and a caller that happens to leave the right value on the stack
# behaves correctly.
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
