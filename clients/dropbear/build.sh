#!/usr/bin/env bash
#
# Build Dropbear's SSH client, dbclient, for m68k AmigaOS 3.x against
# bsdsocket.library.
#
#   clients/dropbear/build.sh [-u] [-b BUILDDIR] [-T] [-P "prog ..."]
#
#   -u  fetch/update third_party/dropbear to the pinned tag first
#   -b  build directory (default build/dropbear)
#   -T  compile Dropbear's TRACE output in (-v at runtime); larger and slower
#   -P  which programs to build (default "dbclient")
#
# WHAT IS BEING BUILT
#
#   Upstream Dropbear from third_party/dropbear, a submodule pinned to the tag
#   in DROPBEAR_TAG below.  NOTHING IN third_party/dropbear IS PATCHED -- there
#   is no counterpart to clients/curl/curl-amitls.patch here, because none was
#   needed.  The entire AmigaOS port is:
#
#     clients/dropbear/localoptions.h     what is compiled in
#     clients/dropbear/amiga_dropbear.c   the syscalls this toolchain lacks
#     clients/dropbear/include/           two shim headers
#     the flags below
#
#   Dropbear is MIT-licensed (third_party/dropbear/LICENSE), with libtomcrypt
#   and libtommath public domain and a handful of OpenSSH files under the
#   2-clause BSD.  All of it is compatible with this tree's MIT posture.
#
# WHY ./configure AND NOT A HAND-WRITTEN config.h
#
#   docs/RESEARCH.md §11.7 recommended a hand-written config for wget, because
#   wget needs `./bootstrap`, which needs autoconf, automake, libtool, gettext
#   and a gnulib checkout on the build host.  Dropbear needs none of that: it
#   ships a GENERATED `configure` in the repository, so a cross-configure is one
#   command and no build-host autotools are involved.  It is also worth more
#   than a hand-written header, because it discovers the gaps by LINKING rather
#   than by someone remembering to list them.
#
#   The one thing it must not see is clients/dropbear/amiga_dropbear.o.  That
#   object defines fork(), getpass(), select() and getpwnam(), so a configure
#   run with it in LIBS would answer HAVE_FORK=1 -- and Dropbear would then
#   compile the fork() paths for a fork() that always fails.  So it is added at
#   MAKE time and not at CONFIGURE time, and config.h therefore describes the
#   toolchain honestly.
#
# THE FLAGS THAT ARE NOT clients/amiga-client.sh's
#
#   --disable-harden
#       Dropbear's configure probes for -fPIE, -fstack-protector-strong and
#       -D_FORTIFY_SOURCE=2 and keeps whatever compiles.  All three "compile"
#       here and the third one is fatal later: _FORTIFY_SOURCE pulls in
#       newlib's <ssp/*.h>, whose __ssp_redirect0 macros do not expand under
#       this GCC, and every subsequent configure test then fails for a reason
#       that has nothing to do with what it was testing.  (That is how
#       netinet/in.h and netdb.h get reported missing when both are present.)
#       None of the three means anything on a machine with no MMU, no ASLR and
#       no stack guard page.
#
#   -DFD_SETSIZE=256
#       clients/dropbear/amiga_dropbear.c maps bsdsocket descriptors to 64..191
#       and a wakeup pipe to 192..193, because both namespaces start at 0 and a
#       program holding a socket and stdin cannot otherwise tell them apart.
#       newlib's fd_set is 64 bits.  See that file.
#
#   -Wl,--wrap=open,--wrap=read,--wrap=write,--wrap=close
#       All four are in newlib's libc.a, in ONE object (lib_a-open.o), so any
#       one of them being referenced drags in all four and none can be
#       redefined.  They have to be intercepted anyway, because on this platform
#       a read() of a socket is recv().  --wrap is also the only route that
#       survives `atomicio(read, ...)`, which passes read as a function pointer;
#       a macro would not have.
#
#       open() is wrapped for a second reason: it is how dbrandom.c reaches
#       DROPBEAR_URANDOM_DEV, which localoptions.h renames to "RANDOM:" and
#       clients/dropbear/amiga_dropbear.c answers from src/common/ami_random.c.
#       Read the entropy note in that file before trusting any of it.
#
#   --disable-{syslog,shadow,lastlog,utmp,utmpx,wtmp,loginfunc,pututline,pututxline}
#       There is no system logger and no account database on AmigaOS 3.x.  Left
#       enabled, configure finds <utmp.h> in the Roadshow NDK -- it is there for
#       AmiTCP compatibility -- and Dropbear then tries to record logins.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)

DROPBEAR_URL="https://github.com/mkj/dropbear.git"
DROPBEAR_TAG="DROPBEAR_2026.94"

BUILD="build/dropbear"
UPDATE=0
TRACE=0
PROGRAMS="dbclient"

while getopts "ub:TP:" opt; do
    case "$opt" in
        u) UPDATE=1 ;;
        b) BUILD="$OPTARG" ;;
        T) TRACE=1 ;;
        P) PROGRAMS="$OPTARG" ;;
        *) echo "usage: $0 [-u] [-b builddir] [-T] [-P \"prog ...\"]" >&2; exit 2 ;;
    esac
done

DB_DIR="$ROOT/third_party/dropbear"

. "$ROOT/clients/amiga-client.sh"

amiga_client_checkout "$DB_DIR" "$DROPBEAR_URL" "$DROPBEAR_TAG" "$UPDATE"

if ! git -C "$DB_DIR" diff --quiet; then
    echo "!! third_party/dropbear has local modifications." >&2
    echo "   This port patches nothing; sort that out before building." >&2
    git -C "$DB_DIR" status --short >&2
    exit 1
fi

echo "==> support archives"
amiga_client_prepare "$ROOT/build/clients"

OUT="$ROOT/$BUILD"
LOG="$OUT-configure.log"
mkdir -p "$OUT"

SHIM_INC="$ROOT/clients/dropbear/include"
DB_CFLAGS="-I$SHIM_INC $AMIGA_CLIENT_CFLAGS -I$AMIGA_NDK -I$ROOT/include -DFD_SETSIZE=256"
[ "$TRACE" = "1" ] && DB_CFLAGS="$DB_CFLAGS -DDEBUG_TRACE=1"

# The shim, built with exactly the flags Dropbear's own objects get, so there
# is one fd_set and one struct timeval across the whole program.
SHIM_O="$ROOT/build/clients/obj/db-amiga_dropbear.o"
mkdir -p "$ROOT/build/clients/obj"
if [ ! -f "$SHIM_O" ] || [ "$ROOT/clients/dropbear/amiga_dropbear.c" -nt "$SHIM_O" ]; then
    echo "  CC amiga_dropbear.c"
    "$AMIGA_GCC" $DB_CFLAGS -Wall -Wextra -c -o "$SHIM_O" \
                 "$ROOT/clients/dropbear/amiga_dropbear.c"
fi

# The machine's one entropy pool, and the timer base it reads the E-Clock
# through.  clients/amiga-client.sh already compiles src/common/ami_udivdi3.c
# into every client, so reaching into src/common for a shared piece is the
# established shape and not a new one -- and the alternative here is worse than
# untidy: a second generator, so that the SSH client's randomness and the TCP
# stack's randomness would have to be argued about separately.
#
# These two are built WITHOUT -D__USE_NEW_TIMEVAL__, which the rest of the
# client needs and they cannot have.  ami_random.c calls GetSysTime(), an
# AmigaOS function taking AmigaOS's `struct TimeVal`; that flag is precisely
# the switch that hands `struct timeval` to libc instead.  They are separate
# translation units that share no type with Dropbear, so the two conventions
# never meet.
AMI_CFLAGS="$AMIGA_CLIENT_ARCH $AMIGA_CLIENT_OPT -fomit-frame-pointer -I$ROOT/include -I$AMIGA_NDK"
SHIM_OBJS=("$SHIM_O")
for c in "$ROOT/src/common/ami_random.c" "$ROOT/src/common/compat.c"; do
    o="$ROOT/build/clients/obj/db-$(basename "${c%.c}").o"
    if [ ! -f "$o" ] || [ "$c" -nt "$o" ]; then
        echo "  CC $(basename "$c")"
        "$AMIGA_GCC" $AMI_CFLAGS -Wall -c -o "$o" "$c"
    fi
    SHIM_OBJS+=("$o")
done

cp "$ROOT/clients/dropbear/localoptions.h" "$OUT/localoptions.h"

# Reconfigure when the flags change; Dropbear caches config.h and a stale one
# is silent.
STAMP="$OUT/.amiga-flags"
if [ ! -f "$OUT/config.h" ] || [ ! -f "$STAMP" ] || \
   [ "$(cat "$STAMP")" != "$DB_CFLAGS" ]; then
    echo "==> configuring dropbear in ${BUILD}"
    (
        cd "$OUT"
        CC="$AMIGA_GCC" \
        AR="$AMIGA_TOOLCHAIN_ROOT/bin/m68k-amigaos-ar" \
        RANLIB="$AMIGA_TOOLCHAIN_ROOT/bin/m68k-amigaos-ranlib" \
        STRIP="$AMIGA_TOOLCHAIN_ROOT/bin/m68k-amigaos-strip" \
        CFLAGS="$DB_CFLAGS" \
        LDFLAGS="$AMIGA_CLIENT_LDFLAGS" \
        LIBS="-Wl,--start-group -lamigaclient -lc -Wl,--end-group" \
        "$DB_DIR/configure" \
            --host=m68k-amigaos \
            --disable-harden \
            --disable-zlib \
            --disable-pam \
            --disable-syslog \
            --disable-shadow \
            --disable-lastlog \
            --disable-utmp --disable-utmpx --disable-wtmp \
            --disable-loginfunc --disable-pututline --disable-pututxline
    ) >"$LOG" 2>&1 || {
        echo "!! configure failed; see $LOG" >&2
        tail -30 "$LOG" >&2
        exit 1
    }
    printf '%s' "$DB_CFLAGS" > "$STAMP"
fi

# The one thing worth failing loudly on.  Without <netinet/in.h> Dropbear does
# not see struct sockaddr_in and quietly builds against fake-rfc2553's stubs,
# which is a client that cannot resolve anything.  It is also the exact symptom
# --disable-harden fixes, so it is checked rather than assumed.
if ! grep -q '^#define HAVE_NETINET_IN_H 1' "$OUT/config.h"; then
    echo "!! dropbear did not find <netinet/in.h> -- see $LOG" >&2
    echo "   (a configure test failed for an unrelated reason and took the" >&2
    echo "    header probes with it; check the hardening flags)" >&2
    exit 1
fi

echo "==> building $PROGRAMS"
make -C "$OUT" -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)" \
     PROGRAMS="$PROGRAMS" \
     LDFLAGS="$AMIGA_CLIENT_LDFLAGS -Wl,--wrap=open,--wrap=read,--wrap=write,--wrap=close" \
     LIBS="${SHIM_OBJS[*]} -Wl,--start-group -lamigaclient -lc -Wl,--end-group"

echo
for p in $PROGRAMS; do
    ls -l "$OUT/$p"
done
echo
"$AMIGA_SIZE" $(for p in $PROGRAMS; do echo "$OUT/$p"; done) 2>/dev/null || true
echo
echo "dbclient for AmigaOS: $BUILD/dbclient"
echo "  kex/cipher set:"
grep -E '^#define DROPBEAR_(CURVE25519|ECDH|DH_GROUP14_SHA256|CHACHA20POLY1305|AES128|AES256|ED25519|ECDSA|RSA) ' \
     "$ROOT/third_party/dropbear/src/default_options.h" | sed 's/^/    /'
