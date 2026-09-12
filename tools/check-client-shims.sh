#!/usr/bin/env bash
#
# The port's shim translation units, compiled with the prototype gate on.
#
#   tools/check-client-shims.sh
#
# clients/dropbear/build.sh puts -Werror=missing-prototypes on these, and that
# build once ran in the release workflow and nowhere else -- so until this
# existed the gate failed a release and never a push.  Every function these
# files define is an entry point somebody else calls: libc symbols the port
# interposes, __wrap_ targets the linker redirects, and the two knobs in
# clients/compat/amiga_compat.h.  A signature that drifts from the declaration
# its callers see is a silently wrong call, not a build failure.
#
# SIX OF THE SEVEN, and the seventh is not an oversight.  amiga_scp.c includes
# "scpmisc.h", which lives in the Dropbear checkout, so it cannot be compiled
# without third_party/dropbear and the generated options.h that go with it.
# That one stays covered by CI's candidate build.  amiga_dropbear.c, which carries
# 33 of the 51 findings this gate was written for, needs nothing but the
# toolchain and the tree.
#
# -fsyntax-only: no objects, no link, and the whole set costs under a second.
# The prototype flag alone and not -Wall: this gate answers one question, and
# the shims still carry two -Wpointer-sign warnings that belong to nobody's
# item yet.  The real build applies -Wall; that is where those live.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"

# shellcheck source=/dev/null
. "$ROOT/clients/amiga-client.sh"

if [ -z "${AMIGA_GCC:-}" ] || [ ! -x "$AMIGA_GCC" ]; then
    echo "client_shims=FAIL reason=no_cross_compiler" >&2
    echo "  \$AMIGA_GCC is '${AMIGA_GCC:-}', which is not an executable." >&2
    echo "  This gate cannot see the thing it checks, so it fails rather" >&2
    echo "  than reporting a pass it has not earned." >&2
    exit 1
fi

SHIMS=(
    clients/dropbear/amiga_dropbear.c
    clients/compat/amiga_posix.c
    clients/compat/amiga_argv.c
    clients/compat/amiga_exit.c
    clients/compat/amiga_libgcc.c
    clients/compat/amiga_net.c
)

# The flags clients/dropbear/build.sh gives amiga_dropbear.c, minus the
# code-generation half that -fsyntax-only does not reach.
DB_CFLAGS=(
    -I"$ROOT/clients/dropbear/include"
    -I"$AMIGA_NDK"
    -I"$ROOT/include"
    -DFD_SETSIZE=256
)

rc=0
for c in "${SHIMS[@]}"; do
    # shellcheck disable=SC2086
    if ! "$AMIGA_GCC" $AMIGA_CLIENT_CFLAGS "${DB_CFLAGS[@]}" \
            $AMIGA_CLIENT_SHIM_WARN -fsyntax-only "$c"; then
        echo "client_shims: $c" >&2
        rc=1
    fi
done

if [ "$rc" -ne 0 ]; then
    echo "client_shims=FAIL files=${#SHIMS[@]}" >&2
    echo "  A shim defines a function no declaration covers.  Declare it in" >&2
    echo "  clients/compat/amiga_compat.h when another translation unit calls" >&2
    echo "  it, or beside the definition with __typeof__ of what it replaces" >&2
    echo "  when the linker's --wrap is what calls it." >&2
    exit 1
fi

echo "client_shims=ok files=${#SHIMS[@]} flag=$AMIGA_CLIENT_SHIM_WARN"
