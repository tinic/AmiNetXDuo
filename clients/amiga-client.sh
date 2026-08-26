#!/usr/bin/env bash
#
# Port a Unix network client to m68k AmigaOS.  Source this, do not run it.
# Exports AMIGA_CLIENT_{CFLAGS,LDFLAGS,LIBDIR,COMPAT_A}; call
# amiga_client_prepare [BUILDROOT] once before configuring.
#
# The three NDK flags are all required: -D__USE_NEW_TIMEVAL__ (NDK vs POSIX
# struct timeval), -D_SYS_MBUF_H (net/if.h has an undefined struct __timeval as
# a FIELD), -include sys/types.h (NDK sys/socket.h uses ssize_t undeclared).
#
# libnet.a and libatomic.a are fabricated because curl's CMakeLists.txt
# hardcodes `-lnet -lm -latomic` for AMIGA and there is no switch for it.
#
# SPDX-License-Identifier: MIT

AMIGA_CLIENT_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

. "$AMIGA_CLIENT_ROOT/tools/amiga-toolchain.sh"

# -m68020 by default, overridden through AMIGA_CLIENT_ARCH.  -O2 rather than the
# -O0 curl's own CI uses: that CI is checking the build works, and this is
# building something somebody has to wait for on a 14 MHz machine.
#
# -fno-strict-aliasing because a great deal of portable C predates the rule
# and a client is not ours to fix.
AMIGA_CLIENT_ARCH="${AMIGA_CLIENT_ARCH:--m68020}"
AMIGA_CLIENT_OPT="${AMIGA_CLIENT_OPT:--O2}"

# -include amiga_compat.h so the shims in clients/compat that newlib has no
# header for, nanosleep(), clearenv(), are declared everywhere.  See that
# file for why it is a forced include and not a patch or a shadowed <time.h>.
AMIGA_CLIENT_CFLAGS="$AMIGA_CLIENT_ARCH $AMIGA_CLIENT_OPT -fomit-frame-pointer -fno-strict-aliasing -D__USE_NEW_TIMEVAL__ -D_SYS_MBUF_H -include sys/types.h -I$AMIGA_CLIENT_ROOT/clients/compat -include amiga_compat.h"

export AMIGA_CLIENT_ROOT AMIGA_CLIENT_ARCH AMIGA_CLIENT_OPT AMIGA_CLIENT_CFLAGS

amiga_client_prepare()
{
    local out
    local obj
    local c
    local o

    out="${1:-$AMIGA_CLIENT_ROOT/build/clients}"
    AMIGA_CLIENT_LIBDIR="$out/lib"
    obj="$out/obj"

    mkdir -p "$AMIGA_CLIENT_LIBDIR" "$obj"

    local sources=(
        "$AMIGA_CLIENT_ROOT/clients/compat/amiga_posix.c"
        "$AMIGA_CLIENT_ROOT/clients/compat/amiga_argv.c"
        "$AMIGA_CLIENT_ROOT/clients/compat/amiga_exit.c"
        "$AMIGA_CLIENT_ROOT/clients/compat/amiga_libgcc.c"
        "$AMIGA_CLIENT_ROOT/src/common/ami_udivdi3.c"
    )

    local objs=()
    for c in "${sources[@]}"; do
        o="$obj/$(basename "${c%.c}").o"
        echo "  CC $(basename "$c")"
        "$AMIGA_GCC" $AMIGA_CLIENT_CFLAGS -Wall -I"$AMIGA_NDK" \
                     -c -o "$o" "$c" || return 1
        objs+=("$o")
    done

    rm -f "$AMIGA_CLIENT_LIBDIR/libamigaclient.a"
    "$AMIGA_TOOLCHAIN_ROOT/bin/m68k-amigaos-ar" rcs \
        "$AMIGA_CLIENT_LIBDIR/libamigaclient.a" "${objs[@]}" || return 1

    # libnet.a: one weak SocketBase, so configure-time socket tests link.
    o="$obj/amiga_net.o"
    echo "  CC amiga_net.c"
    "$AMIGA_GCC" $AMIGA_CLIENT_CFLAGS -Wall -I"$AMIGA_NDK" \
                 -c -o "$o" "$AMIGA_CLIENT_ROOT/clients/compat/amiga_net.c" || return 1
    rm -f "$AMIGA_CLIENT_LIBDIR/libnet.a"
    "$AMIGA_TOOLCHAIN_ROOT/bin/m68k-amigaos-ar" rcs \
        "$AMIGA_CLIENT_LIBDIR/libnet.a" "$o" || return 1

    # libatomic.a: empty on purpose.  amiga_libgcc.c has the one atomic.
    o="$obj/amiga_empty.o"
    : > "$obj/amiga_empty.c"
    "$AMIGA_GCC" $AMIGA_CLIENT_ARCH -c -o "$o" "$obj/amiga_empty.c" || return 1
    rm -f "$AMIGA_CLIENT_LIBDIR/libatomic.a"
    "$AMIGA_TOOLCHAIN_ROOT/bin/m68k-amigaos-ar" rcs \
        "$AMIGA_CLIENT_LIBDIR/libatomic.a" "$o" || return 1

    # crt0.o, repaired.  See clients/compat/fix-crt0.py: the stock one hands
    # main() the ADDRESS of __argv instead of __argv, so every ported client
    # sees an argv of empty strings and a NULL in the middle of it.  Our own
    # commands never noticed because they read arguments through ReadArgs().
    AMIGA_CLIENT_STARTFILE="$AMIGA_CLIENT_LIBDIR/crt0.o"
    local stock="$AMIGA_TOOLCHAIN_ROOT/m68k-amigaos/lib/crt0.o"
    echo "  FIX crt0.o"
    python3 "$AMIGA_CLIENT_ROOT/clients/compat/fix-crt0.py" \
            "$stock" "$AMIGA_CLIENT_STARTFILE" || return 1

    AMIGA_CLIENT_COMPAT_A="$AMIGA_CLIENT_LIBDIR/libamigaclient.a"
    # --wrap=main: the crt0 does not tokenise the CLI command line into argv[],
    # so route main() through clients/compat/amiga_argv.c, which rebuilds a real
    # POSIX argv[] from dos.library and runs the client on its private stack.
    # --wrap=exit,_exit: so that shim can put the task's stack bounds back when a
    # client ends by exit()ing off the swapped stack instead of returning.
    AMIGA_CLIENT_LDFLAGS="$AMIGA_CLIENT_ARCH -nostartfiles $AMIGA_CLIENT_STARTFILE -L$AMIGA_CLIENT_LIBDIR -Wl,--wrap=main,--wrap=exit,--wrap=_exit"

    export AMIGA_CLIENT_LIBDIR AMIGA_CLIENT_COMPAT_A AMIGA_CLIENT_LDFLAGS
    export AMIGA_CLIENT_STARTFILE
    return 0
}

# A client's own source may live in third_party/ as a submodule or be cloned
# on demand, exactly as tests/conformance/build.sh does for bsdsocktest.  The
# pin is the submodule's recorded commit; -u is the only way past it.
amiga_client_checkout()
{
    local dir="$1"
    local url="$2"
    local ref="$3"
    local update="${4:-0}"

    if [ ! -e "$dir/.git" ]; then
        echo "==> fetching $url into ${dir#$AMIGA_CLIENT_ROOT/}"
        mkdir -p "$(dirname "$dir")"
        git clone --quiet "$url" "$dir" || return 1
        git -C "$dir" checkout --quiet "$ref" || return 1
    elif [ "$update" = "1" ]; then
        git -C "$dir" fetch --quiet --tags origin || return 1
        git -C "$dir" checkout --quiet "$ref" || return 1
    fi

    local at
    at=$(git -C "$dir" rev-parse --short HEAD)
    echo "==> ${dir#$AMIGA_CLIENT_ROOT/} at $ref ($at)"
    return 0
}
