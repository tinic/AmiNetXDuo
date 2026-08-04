#!/usr/bin/env bash
#
# Port a Unix network client to m68k AmigaOS.  Source this, do not run it:
#
#   . "$ROOT/clients/amiga-client.sh"
#
# It resolves the cross toolchain (through tools/amiga-toolchain.sh, so a
# client build and a stack build never disagree about which compiler they are
# using) and then exports everything a stock configure or CMake needs to
# produce a working AmigaOS binary against bsdsocket.library:
#
#   AMIGA_CLIENT_CFLAGS     compiler flags, including the three NDK
#                           workarounds explained below
#   AMIGA_CLIENT_LDFLAGS    -L for the fabricated support archives
#   AMIGA_CLIENT_LIBDIR     where those archives are
#   AMIGA_CLIENT_COMPAT_A   libamigaclient.a, the libc and libgcc gaps
#
# amiga_client_prepare [BUILDROOT]  builds the archives.  Call it once before
# configuring; it is cheap and idempotent.
#
# ----------------------------------------------------------------------------
# THE THREE FLAGS, AND WHY EACH IS THERE
#
#   -D__USE_NEW_TIMEVAL__
#       The Roadshow NDK's <devices/timer.h> defines `struct timeval` with
#       AmigaOS semantics (two ULONGs, tv_secs/tv_micro).  newlib defines the
#       POSIX one (time_t/suseconds_t, tv_sec/tv_usec).  Including both is a
#       redefinition error, and every Unix client wants the POSIX one.  The
#       NDK header provides this switch itself, in as many words: define it
#       and AmigaOS uses `struct TimeVal` instead and leaves `struct timeval`
#       to libc.  Nothing is being worked around here; it is the supported
#       route and it has been there since NDK 3.9.
#
#   -D_SYS_MBUF_H
#       <proto/bsdsocket.h> pulls in <sys/mbuf.h>, which pulls in <net/if.h>,
#       which has `struct __timeval ifi_lastchange;` as a FIELD, and
#       `struct __timeval` is never defined anywhere in the NDK.  It is an
#       opaque type the inline stubs use behind a pointer, so a pointer is
#       fine and a field is not.  Nothing in a client wants mbufs, so the
#       whole header is suppressed by its own guard.  (curl's own
#       lib/curl_setup.h does exactly this, and calls the m_len clash its
#       reason; the incomplete type is the one that actually bites here.)
#
#   -include sys/types.h
#       The NDK's <sys/socket.h> declares recv/send/sendto/... as returning
#       ssize_t without including anything that declares ssize_t.  On the
#       bebbo/clib2 toolchain curl's build is upstream-tested against, some
#       other header has already done it.  Here it has not, so
#       proto/bsdsocket.h fails to compile at all, which a configure script
#       reports as "AmigaOS bsdsocket.library not found" and then silently
#       builds a client that calls a libc networking API that does not exist.
#       tests/conformance/build.sh carries the identical line for the
#       identical reason.
#
# ----------------------------------------------------------------------------
# THE TWO ARCHIVES
#
#   libamigaclient.a    clients/compat/*.c plus src/common/ami_udivdi3.c --
#                       stat/mkdir/isatty/gettimeofday over dos.library, and
#                       the libgcc helpers the zero-byte libgcc.a lacks.  See
#                       those files; each says what it is faithful about.
#
#   libnet.a, libatomic.a
#                       Named, not chosen: curl's CMakeLists.txt hardcodes
#                       `list(APPEND CURL_NETWORK_AND_TIME_LIBS "net" "m"
#                       "atomic")` for AMIGA and there is no switch for it.
#                       libm.a is real and in the toolchain.  The other two
#                       are ours: libnet.a holds a weak SocketBase (see
#                       clients/compat/amiga_net.c, it is what makes
#                       configure-time socket feature tests LINK, which is
#                       not optional), and libatomic.a is empty because
#                       clients/compat/amiga_libgcc.c supplies the one atomic
#                       a client actually reaches.
#
#   Fabricating an archive to satisfy a hardcoded -l is preferable to
#   patching the client's build system: the patch would have to be rebased
#   on every version bump, and this does not.
#
# SPDX-License-Identifier: MIT

AMIGA_CLIENT_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

. "$AMIGA_CLIENT_ROOT/tools/amiga-toolchain.sh"

# -m68020 is the project floor (docs/RESEARCH.md §9).  -O2 rather than the
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
        "$AMIGA_CLIENT_ROOT/clients/compat/amiga_libgcc.c"
        "$AMIGA_CLIENT_ROOT/src/common/ami_udivdi3.c"
    )

    local objs=()
    for c in "${sources[@]}"; do
        o="$obj/$(basename "${c%.c}").o"
        if [ ! -f "$o" ] || [ "$c" -nt "$o" ]; then
            echo "  CC $(basename "$c")"
            "$AMIGA_GCC" $AMIGA_CLIENT_CFLAGS -Wall -I"$AMIGA_NDK" \
                         -c -o "$o" "$c" || return 1
        fi
        objs+=("$o")
    done

    rm -f "$AMIGA_CLIENT_LIBDIR/libamigaclient.a"
    "$AMIGA_TOOLCHAIN_ROOT/bin/m68k-amigaos-ar" rcs \
        "$AMIGA_CLIENT_LIBDIR/libamigaclient.a" "${objs[@]}" || return 1

    # libnet.a: one weak SocketBase, so configure-time socket tests link.
    o="$obj/amiga_net.o"
    if [ ! -f "$o" ] || [ "$AMIGA_CLIENT_ROOT/clients/compat/amiga_net.c" -nt "$o" ]; then
        echo "  CC amiga_net.c"
        "$AMIGA_GCC" $AMIGA_CLIENT_CFLAGS -Wall -I"$AMIGA_NDK" \
                     -c -o "$o" "$AMIGA_CLIENT_ROOT/clients/compat/amiga_net.c" || return 1
    fi
    rm -f "$AMIGA_CLIENT_LIBDIR/libnet.a"
    "$AMIGA_TOOLCHAIN_ROOT/bin/m68k-amigaos-ar" rcs \
        "$AMIGA_CLIENT_LIBDIR/libnet.a" "$o" || return 1

    # libatomic.a: empty on purpose.  amiga_libgcc.c has the one atomic.
    o="$obj/amiga_empty.o"
    if [ ! -f "$o" ]; then
        : > "$obj/amiga_empty.c"
        "$AMIGA_GCC" $AMIGA_CLIENT_ARCH -c -o "$o" "$obj/amiga_empty.c" || return 1
    fi
    rm -f "$AMIGA_CLIENT_LIBDIR/libatomic.a"
    "$AMIGA_TOOLCHAIN_ROOT/bin/m68k-amigaos-ar" rcs \
        "$AMIGA_CLIENT_LIBDIR/libatomic.a" "$o" || return 1

    # crt0.o, repaired.  See clients/compat/fix-crt0.py: the stock one hands
    # main() the ADDRESS of __argv instead of __argv, so every ported client
    # sees an argv of empty strings and a NULL in the middle of it.  Our own
    # commands never noticed because they read arguments through ReadArgs().
    AMIGA_CLIENT_STARTFILE="$AMIGA_CLIENT_LIBDIR/crt0.o"
    local stock="$AMIGA_TOOLCHAIN_ROOT/m68k-amigaos/lib/crt0.o"
    if [ ! -f "$AMIGA_CLIENT_STARTFILE" ] || [ "$stock" -nt "$AMIGA_CLIENT_STARTFILE" ] \
       || [ "$AMIGA_CLIENT_ROOT/clients/compat/fix-crt0.py" -nt "$AMIGA_CLIENT_STARTFILE" ]; then
        echo "  FIX crt0.o"
        python3 "$AMIGA_CLIENT_ROOT/clients/compat/fix-crt0.py" \
                "$stock" "$AMIGA_CLIENT_STARTFILE" || return 1
    fi

    AMIGA_CLIENT_COMPAT_A="$AMIGA_CLIENT_LIBDIR/libamigaclient.a"
    # --wrap=main: the crt0 does not tokenise the CLI command line into argv[],
    # so route main() through clients/compat/amiga_argv.c, which rebuilds a real
    # POSIX argv[] from dos.library and runs the client on a 256 KB stack.
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
