#!/usr/bin/env bash
#
# Build Dropbear's SSH client, dbclient, for m68k AmigaOS 3.x against
# bsdsocket.library.
#
#   clients/dropbear/build.sh [-u] [-b BUILDDIR] [-T] [-p] [-O FILE]
#                             [-P "prog ..."]
#
#   -u  fetch/update third_party/dropbear to the pinned tag first
#   -b  build directory (default build/dropbear)
#   -T  compile Dropbear's TRACE output in (-v at runtime); larger and slower
#   -p  link clients/dropbear/dbprofile.c: --wraps the crypto entry points and
#       prints an E-Clock breakdown of the connection when the process exits
#   -S  STOCK 25519: leave Dropbear's TweetNaCl in place.  The default is to
#       --wrap the four curve25519.c entry points onto src/crypto68k's
#       eight-limb implementation, which is where docs/RESEARCH.md 35's
#       speed-up comes from.  -S is the other arm of that A/B and nothing
#       else; it is not a fallback for a bug.
#   -O  use a different localoptions.h (clients/dropbear/localoptions-*.h are
#       the algorithm-set variants docs/RESEARCH.md 35 compares)
#   -P  which programs to build (default "dbclient")
#
# WHAT IS BEING BUILT
#
#   Upstream Dropbear from third_party/dropbear, a submodule pinned to the tag
#   in DROPBEAR_TAG below.  NOTHING IN third_party/dropbear IS PATCHED, the
#   submodule is built as-is.  The entire AmigaOS port is:
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
#   run with it in LIBS would answer HAVE_FORK=1, and Dropbear would then
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
#   -Wl,--wrap=spawn_command
#       dbutil.c's spawn_command() is three pipes and a fork(), and it is the
#       only place Dropbear starts a program.  The AmigaOS answer is
#       SystemTagList(), which is a different shape entirely, so the whole
#       function is replaced rather than propped up, see THE SERVER RUNS A
#       COMMAND in clients/dropbear/amiga_dropbear.c.
#
#       It is wrapped for both programs because there is one link line, and
#       that is harmless: the client's only caller is -J proxycmd, which
#       localoptions.h compiles out, so dbclient does not reference the symbol
#       at all.
#
#   -Wl,--wrap=getenv,--wrap=ioctl,--wrap=signal
#       getenv() answers TERM (and reads ENV:) from dos.library.  ioctl()
#       answers TIOCGWINSZ from the console's real size so the server is told
#       the true terminal geometry instead of a fixed 80x25.  signal() is wrapped
#       only to capture Dropbear's SIGWINCH handler, which the resize detector
#       calls by hand when the console window changes size, AmigaOS has no such
#       signal.  See con_query_size()/con_child() in amiga_dropbear.c.
#
#   --disable-{syslog,shadow,lastlog,utmp,utmpx,wtmp,loginfunc,pututline,pututxline}
#       There is no system logger and no account database on AmigaOS 3.x.  Left
#       enabled, configure finds <utmp.h> in the Roadshow NDK, it is there for
#       AmiTCP compatibility, and Dropbear then tries to record logins.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)

DROPBEAR_URL="https://github.com/mkj/dropbear.git"
DROPBEAR_TAG="DROPBEAR_2026.94"

BUILD="build/dropbear"
UPDATE=0
TRACE=0
PROFILE=0
STOCK25519=0
KEEP_SYMBOLS=0
LOCALOPTS=""
PROGRAMS="dbclient"

while getopts "ub:TpSkO:P:" opt; do
    case "$opt" in
        u) UPDATE=1 ;;
        b) BUILD="$OPTARG" ;;
        T) TRACE=1 ;;
        p) PROFILE=1 ;;
        S) STOCK25519=1 ;;
        k) KEEP_SYMBOLS=1 ;;
        O) LOCALOPTS="$OPTARG" ;;
        P) PROGRAMS="$OPTARG" ;;
        *) echo "usage: $0 [-u] [-b builddir] [-T] [-p] [-S] [-k] [-O localoptions.h] [-P \"prog ...\"]" >&2; exit 2 ;;
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

# One tree per architecture.  See the note at the top of this file about what
# sharing it cost.
CLIENT_TAG=$(printf '%s' "$AMIGA_CLIENT_ARCH" | tr -d ' =-')
CLIENT_ROOT="$ROOT/build/clients/$CLIENT_TAG"
CLIENT_OBJ="$CLIENT_ROOT/obj"
mkdir -p "$CLIENT_OBJ"

echo "==> support archives ($AMIGA_CLIENT_ARCH)"
amiga_client_prepare "$CLIENT_ROOT"

OUT="$ROOT/$BUILD"
LOG="$OUT-configure.log"
mkdir -p "$OUT"

SHIM_INC="$ROOT/clients/dropbear/include"
DB_CFLAGS="-I$SHIM_INC $AMIGA_CLIENT_CFLAGS -I$AMIGA_NDK -I$ROOT/include -DFD_SETSIZE=256"
[ "$TRACE" = "1" ] && DB_CFLAGS="$DB_CFLAGS -DDEBUG_TRACE=1"

# The server, if one is being built, does not fork.
#
# svr-main.c forks a child per accepted connection.  fork() on AmigaOS fails
# and always will (clients/dropbear/amiga_dropbear.c says why), and svr-main
# treats that as "log a warning, drop the connection", so an unmodified
# build would accept every connection and immediately hang up on it.
#
# DEBUG_NOFORK sets fork_ret to 0 instead of calling fork(), which takes the
# child branch in the SAME process: it closes the listening sockets and runs
# the session inline.  That is one connection per invocation and then exit,
# which is the only shape a machine without fork can offer, and it is a real
# server rather than a broken one.  The DEBUG_ prefix is upstream's name for
# the switch; there is nothing debug-only about what it does here.
case " $PROGRAMS " in
    *" dropbear "*) DB_CFLAGS="$DB_CFLAGS -DDEBUG_NOFORK=1" ;;
esac

# The shim, built with exactly the flags Dropbear's own objects get, so there
# is one fd_set and one struct timeval across the whole program.
SHIM_O="$CLIENT_OBJ/db-amiga_dropbear.o"
echo "  CC amiga_dropbear.c"
"$AMIGA_GCC" $DB_CFLAGS -Wall -Wextra -c -o "$SHIM_O" \
             "$ROOT/clients/dropbear/amiga_dropbear.c"

# The machine's one entropy pool, and the timer base it reads the E-Clock
# through.  clients/amiga-client.sh already compiles src/common/ami_udivdi3.c
# into every client, so reaching into src/common for a shared piece is the
# established shape and not a new one, and the alternative here is worse than
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
    o="$CLIENT_OBJ/db-$(basename "${c%.c}").o"
    echo "  CC $(basename "$c")"
    "$AMIGA_GCC" $AMI_CFLAGS -Wall -c -o "$o" "$c"
    SHIM_OBJS+=("$o")
done

# -O names a REPLACEMENT localoptions.h.  clients/dropbear/localoptions-*.h are
# the algorithm-set variants the profiling work compares; the default is the
# shipping one.  Copied only when it differs, because `cp` moves the mtime and
# Dropbear rebuilds every object that includes it.
LOCALOPTS="${LOCALOPTS:-$ROOT/clients/dropbear/localoptions.h}"
[ -f "$LOCALOPTS" ] || { echo "!! no such localoptions.h: $LOCALOPTS" >&2; exit 2; }
cmp -s "$LOCALOPTS" "$OUT/localoptions.h" || cp "$LOCALOPTS" "$OUT/localoptions.h"

# -p adds clients/dropbear/dbprofile.c, which --wraps the crypto entry points
# and prints a call-count and E-Clock table when the process exits.  The wrap
# list has to match the algorithms actually compiled in: with
# DROPBEAR_CURVE25519 0 the symbol dropbear_curve25519_scalarmult does not
# exist, and --wrap on an absent symbol is an undefined reference to
# __real_<it>.  So the same switches drive the -D and the -Wl on one line each.
PROF_WRAPS=""
if [ "$PROFILE" = "1" ]; then
    # prof_opt DROPBEAR_MACRO DBPROF_MACRO SYM...
    #   on unless localoptions.h switches the algorithm off, in which case both
    #   the -D and the --wrap have to go, together.
    prof_opt() {
        local dbmacro="$1" profmacro="$2" s
        shift 2
        if grep -qE "^#define $dbmacro 0" "$LOCALOPTS"; then
            PROF_CFLAGS="$PROF_CFLAGS -D$profmacro=0"
        else
            PROF_CFLAGS="$PROF_CFLAGS -D$profmacro=1"
            for s in "$@"; do PROF_WRAPS="$PROF_WRAPS,--wrap=$s"; done
        fi
    }
    PROF_CFLAGS=""
    if [ "$STOCK25519" = "0" ]; then
        # amiga_25519.o owns __wrap_dropbear_*, so the instrument attaches one
        # level down, to what that file calls.
        PROF_CFLAGS="$PROF_CFLAGS -DDBPROF_FAST25519=1"
        PROF_CFLAGS="$PROF_CFLAGS -DDBPROF_CURVE25519=0 -DDBPROF_ED25519=0"
        PROF_WRAPS="$PROF_WRAPS,--wrap=c68k_x25519"
        PROF_WRAPS="$PROF_WRAPS,--wrap=c68k_ed25519_sign,--wrap=c68k_ed25519_verify"
    else
        PROF_CFLAGS="$PROF_CFLAGS -DDBPROF_FAST25519=0"
        prof_opt DROPBEAR_CURVE25519 DBPROF_CURVE25519 dropbear_curve25519_scalarmult
        prof_opt DROPBEAR_ED25519    DBPROF_ED25519    dropbear_ed25519_sign dropbear_ed25519_verify
    fi
    prof_opt DROPBEAR_ECDH       DBPROF_ECC        ltc_ecc_mulmod dropbear_ecc_shared_secret mp_invmod
    prof_opt DROPBEAR_RSA        DBPROF_RSA        mp_exptmod
    PROF_WRAPS="$PROF_WRAPS,--wrap=sha512_process,--wrap=sha256_process"
    PROF_WRAPS="$PROF_WRAPS,--wrap=chacha_crypt,--wrap=poly1305_process,--wrap=select"
    PROF_WRAPS="$PROF_WRAPS,--wrap=exit"

    PROF_O="$CLIENT_OBJ/db-dbprofile-$(basename "$BUILD").o"
    echo "  CC dbprofile.c"
    "$AMIGA_GCC" $DB_CFLAGS $PROF_CFLAGS -Wall -Wextra -c -o "$PROF_O" \
                 "$ROOT/clients/dropbear/dbprofile.c"
    SHIM_OBJS+=("$PROF_O")
fi

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
    echo "!! dropbear did not find <netinet/in.h>, see $LOG" >&2
    echo "   (a configure test failed for an unrelated reason and took the" >&2
    echo "    header probes with it; check the hardening flags)" >&2
    exit 1
fi

# dbprofile.o's __real_SYM references come AFTER libtomcrypt.a and
# libtommath.a on the link line, and by then those archives have been scanned:
# every reference from Dropbear's own objects was renamed to __wrap_SYM, so
# nothing pulled chacha_crypt.o or bn_mp_invmod.o out.  Naming the two archives
# a second time, after the shim, is the ordinary answer to an archive that is
# needed twice.
PROF_LIBS=""
if [ "$PROFILE" = "1" ]; then
    PROF_LIBS="$OUT/libtomcrypt/libtomcrypt.a $OUT/libtommath/libtommath.a"
fi

# ------------------------------------------------- the fast 25519 ---------
#
# AFTER configure, not before: amiga_25519.c includes Dropbear's includes.h,
# which includes the config.h that configure writes.  Building it earlier fails
# on a fresh build directory and succeeds on a stale one, which is the worst of
# both.
#
# src/crypto68k/c68k_25519.c is plain C over <stdint.h> and touches no AmigaOS
# type, so it takes the client flags unchanged.  The glue that binds it to
# Dropbear's four exported names is clients/dropbear/amiga_25519.c, and it
# needs Dropbear's own headers, includes.h, curve25519.h, dbrandom.h and
# libtomcrypt's SHA-512, so it is compiled with the Dropbear flags rather
# than the bare ones.
FAST_WRAPS=""
if [ "$STOCK25519" = "0" ]; then
    # Which field multiply this CPU gets, the same rule
    # src/crypto68k/CMakeLists.txt applies: 68020/68040 have MULU.L's
    # 32x32 -> 64 form and use it, the 68000 never had it and the 68060
    # dropped it, so both build the product from four MULU.W.  The define
    # reaches the .c as well, because that is where the choice between the
    # assembly and the portable C is made.
    #
    # dbclient compiles c68k_25519.c itself rather than linking the crypto68k
    # archive, so the .S has to be assembled here too.
    #
    # AMINETXDUO_CLIENT_ANY=1 builds ONE ssh for the whole family instead:
    # -m68000 code, both halves of c68k_25519.S assembled, and the choice made
    # from AttnFlags at the first handshake (clients/dropbear/amiga_25519.c).
    # It is the same arrangement AMINETXDUO_CPU=any gives the two libraries,
    # and it matters more here than anywhere: an ssh handshake is 97% public
    # key arithmetic (docs/RESEARCH.md 35), so the wrong binary is not a few
    # percent, it is the difference between 12 s and 84 s.
    case "$AMIGA_CLIENT_ARCH" in
        *68020*|*68030*|*68040*) C68K_25519_ASM="-DC68K_ASM=1" ;;
        *68000*|*68010*|*68060*) C68K_25519_ASM="-DC68K_ASM_MULW=1" ;;
        *)                       C68K_25519_ASM="" ;;
    esac
    if [ -n "${AMINETXDUO_CLIENT_ANY:-}" ]; then
        C68K_25519_ASM="-DC68K_MV=1"
    fi

    # AMINETXDUO_25519_NOASM=1 takes the portable C on any part.  It is the
    # other arm of the A/B the assembly has to justify itself against, and it
    # is the supported way to bisect a suspected assembly bug -- the posture
    # src/crypto68k/CMakeLists.txt already takes for AMINETXDUO_CRYPTO68K_ASM.
    [ -n "${AMINETXDUO_25519_NOASM:-}" ] && C68K_25519_ASM=""
    :

    C68K_O="$CLIENT_OBJ/db-c68k_25519.o"
    echo "  CC c68k_25519.c ${C68K_25519_ASM:--DC68K_NO_ASM}"
    "$AMIGA_GCC" $AMI_CFLAGS $C68K_25519_ASM -I"$ROOT/src/crypto68k" \
                 -Wall -Wextra -c -o "$C68K_O" \
                 "$ROOT/src/crypto68k/c68k_25519.c"
    SHIM_OBJS+=("$C68K_O")

    if [ -n "${AMINETXDUO_CLIENT_ANY:-}" ]; then
        # Both halves, each assembled for what it needs and named apart, the
        # way src/crypto68k/CMakeLists.txt does it for tls.library.
        for half in "mulu:-DC68K_ASM=1:-m68020" "mulw:-DC68K_ASM_MULW=1:-m68000"; do
            sfx="${half%%:*}"; rest="${half#*:}"
            hdef="${rest%%:*}"; harch="${rest##*:}"
            C68K_ASM_O="$CLIENT_OBJ/db-c68k_25519_$sfx.o"
            echo "  AS c68k_25519.S $hdef $harch"
            "$AMIGA_GCC" $harch $hdef -DC68K_MV=1 -DC68K_MV_SUFFIX="_$sfx" \
                         -I"$ROOT/src/crypto68k" -c -o "$C68K_ASM_O" \
                         "$ROOT/src/crypto68k/c68k_25519.S"
            SHIM_OBJS+=("$C68K_ASM_O")
        done
    elif [ -n "$C68K_25519_ASM" ]; then
        C68K_ASM_O="$CLIENT_OBJ/db-c68k_25519_asm.o"
        echo "  AS c68k_25519.S $C68K_25519_ASM"
        "$AMIGA_GCC" $AMIGA_CLIENT_ARCH $C68K_25519_ASM \
                     -I"$ROOT/src/crypto68k" -c -o "$C68K_ASM_O" \
                     "$ROOT/src/crypto68k/c68k_25519.S"
        SHIM_OBJS+=("$C68K_ASM_O")
    fi

    # default_options_guard.h is generated by MAKE, not by configure, and
    # includes.h pulls it in.  Asking make for that one target first is all it
    # takes; the alternative, hand-declaring what this file needs instead of
    # including Dropbear's own headers, would drop the compile-time check
    # that the four signatures still match the pinned tag.
    make -C "$OUT" default_options_guard.h >/dev/null

    G25519_O="$CLIENT_OBJ/db-amiga_25519-$(basename "$BUILD").o"
    echo "  CC amiga_25519.c"
    # -I"$OUT" FIRST: config.h, localoptions.h and the generated
    # default_options_guard.h all live in the build directory, and Dropbear's
    # own objects are compiled with the same precedence.
    "$AMIGA_GCC" -I"$OUT" $DB_CFLAGS -I"$ROOT/src/crypto68k" \
                 -I"$DB_DIR/src" -I"$DB_DIR/libtomcrypt/src/headers" \
                 -I"$DB_DIR/libtommath" -DLOCALOPTIONS_H_EXISTS \
                 -DDROPBEAR_CLIENT -Wall -c -o "$G25519_O" \
                 "$ROOT/clients/dropbear/amiga_25519.c"
    SHIM_OBJS+=("$G25519_O")

    FAST_WRAPS=",--wrap=dropbear_curve25519_scalarmult"
    FAST_WRAPS="$FAST_WRAPS,--wrap=dropbear_ed25519_make_key"
    FAST_WRAPS="$FAST_WRAPS,--wrap=dropbear_ed25519_sign"
    FAST_WRAPS="$FAST_WRAPS,--wrap=dropbear_ed25519_verify"
fi


# ALWAYS RELINK, because our objects are not prerequisites of Dropbear's
# dbclient target.  They arrive through LIBS, which is a variable, so make
# compares dbclient against its own objects only, finds nothing to do, and
# hands back a binary built before the change being tested -- which looks
# exactly like the change not working, and cost a whole emulator run to
# notice.  The support objects above are rebuilt on every invocation, so
# there is nothing to compare against anyway: remove the program and let the
# link happen.
for p in $PROGRAMS; do
    rm -f "$OUT/$p"
done

# A linker map, because tools/profiler/profreport.py cannot name a file-static
# function without one: the linker drops those from HUNK_SYMBOL, and the only
# route back is where the map placed each object plus nm on that object.
# Without it every sample in a static lands silently on the preceding global
# and the ranking looks plausible and is wrong.
#
# One make invocation runs one link per program and every link writes the same
# -Wl,-Map path, so the map would describe whichever program linked last.
# Offer it for a single program only and say so otherwise.
MAP_FLAG=""
if [ "$(set -- $PROGRAMS; echo $#)" = "1" ]; then
    MAP_FLAG=" -Wl,-Map=$OUT/$PROGRAMS.map"
else
    echo "==> $PROGRAMS: more than one program, so no linker map is written"
fi

echo "==> building $PROGRAMS"
make -C "$OUT" -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)" \
     PROGRAMS="$PROGRAMS" \
     LDFLAGS="$AMIGA_CLIENT_LDFLAGS -Wl,--wrap=open,--wrap=read,--wrap=write,--wrap=close,--wrap=spawn_command,--wrap=getenv,--wrap=ioctl,--wrap=signal$FAST_WRAPS$PROF_WRAPS$MAP_FLAG" \
     LIBS="${SHIM_OBJS[*]} $PROF_LIBS -Wl,--start-group -lamigaclient -lc -Wl,--end-group"

# Symbols, before the sizes are reported so the numbers are the ones that ship.
# Dropbear has a `strip` target but nothing invokes it, and STRIP= reaches only
# `make install`, which this build never runs -- so dbclient carried 935 symbols
# and 21,648 bytes into every release.  The Amiga reads none of them: a profile
# of this binary is built on the host from the objects in $OUT.  -k keeps them.
if [ "$KEEP_SYMBOLS" = "0" ]; then
    for p in $PROGRAMS; do
        [ -f "$OUT/$p" ] && "$AMIGA_TOOLCHAIN_ROOT/bin/m68k-amigaos-strip" "$OUT/$p"
    done
fi

echo
for p in $PROGRAMS; do
    ls -l "$OUT/$p"
done
echo
"$AMIGA_SIZE" $(for p in $PROGRAMS; do echo "$OUT/$p"; done) 2>/dev/null || true
echo
echo "dbclient for AmigaOS: $BUILD/dbclient"
echo "  kex/cipher set (localoptions.h wins over default_options.h):"
# Reporting default_options.h alone was wrong the moment -O existed: it says
# CURVE25519 1 for a build that has none.  So the override file is read first
# and the default only fills what it does not mention.
for m in CURVE25519 ECDH DH_GROUP14_SHA256 CHACHA20POLY1305 AES128 AES256 \
         ED25519 ECDSA RSA; do
    # || true on both: under set -e a grep that matches nothing is fatal, and
    # matching nothing is the NORMAL case here, localoptions.h only names the
    # options it overrides.  Without this the loop dies on its first iteration,
    # which is exactly what it did, silently, for as long as every caller piped
    # this script's output somewhere and lost the exit status.
    v=$(grep -E "^#define DROPBEAR_$m " "$OUT/localoptions.h" 2>/dev/null | awk '{print $3}' || true)
    [ -n "$v" ] || v=$(grep -E "^#define DROPBEAR_$m " \
                       "$ROOT/third_party/dropbear/src/default_options.h" \
                       2>/dev/null | awk '{print $3}' || true)
    [ -n "$v" ] || v="(unset)"
    printf '    %-28s %s\n' "DROPBEAR_$m" "$v"
done
