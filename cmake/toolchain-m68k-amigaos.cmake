# CMake toolchain for m68k-amigaos-gcc (AmigaPorts / newlib).
#
# Usage:
#   cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-m68k-amigaos.cmake
#
# Override the toolchain root with -DAMIGA_TOOLCHAIN_ROOT=<path> if it is not the
# default location used by the local amigaos/ checkout.

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR m68k)

# Search order, first hit wins.  Kept identical to tools/amiga-toolchain.sh so
# that a shell script and a CMake configure never pick different compilers:
#
#   1. -DAMIGA_TOOLCHAIN_ROOT / $AMIGA_TOOLCHAIN_ROOT  , explicit
#   2. the PINNED tree in the fetch cache              , what CI uses
#   3. m68k-amigaos-gcc on $PATH                       , container, module
#   4. /opt/m68k-amigaos                               , crosstools layout
#   5. $HOME/amigaos/tools/m68k-amigaos-gcc            , local default
#
# Before this list existed there was only entry 5, which meant a clean checkout
# on any machine but one configured a compiler that was not there and failed
# with a CMake internal error rather than an explanation.
#
# 2 through 5 must also RUN here, not merely exist.  The fetch cache holds a
# linux/amd64 tree and can be populated on a host that cannot execute it (a
# Mac, say, where the headers and the pin are still worth having), and an
# existence test alone would rank that ahead of a working native install.
if(NOT AMIGA_TOOLCHAIN_ROOT)
    if(DEFINED ENV{AMIGA_TOOLCHAIN_ROOT})
        set(AMIGA_TOOLCHAIN_ROOT "$ENV{AMIGA_TOOLCHAIN_ROOT}")
    else()
        if(DEFINED ENV{AMINETXDUO_TOOLCHAIN_CACHE})
            set(_amiga_cache "$ENV{AMINETXDUO_TOOLCHAIN_CACHE}")
        elseif(DEFINED ENV{XDG_CACHE_HOME})
            set(_amiga_cache "$ENV{XDG_CACHE_HOME}/aminetxduo/toolchain")
        else()
            set(_amiga_cache "$ENV{HOME}/.cache/aminetxduo/toolchain")
        endif()

        # THE CACHE CANDIDATE IS THE PIN, NOT <cache>/current.  tools/
        # amiga-toolchain.sh carries the long version; the short one is that
        # `current` is a symlink tools/fetch-toolchain.sh writes when it
        # INSTALLS, so a cache that already holds the pinned tree leaves it
        # addressing whatever was current last and nothing ever compared the
        # two.  The self-hosted emulator runner held both eabb6789378f (the
        # pin, GCC 16.2.0b) and c63033fd4473 (GCC 15.2) with `current` on the
        # older one, and every cross build there ran under GCC 15.2.
        #
        # --print-root prints <cache>/<sha12> for this platform and touches no
        # network.  It exits non-zero on a host with no published asset, where
        # there is no pin to compare anything to.
        set(_amiga_pin "")
        execute_process(
            COMMAND "${CMAKE_CURRENT_LIST_DIR}/../tools/fetch-toolchain.sh"
                    --print-root
            OUTPUT_VARIABLE _amiga_pin
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
            RESULT_VARIABLE _amiga_pin_rc)
        if(NOT _amiga_pin_rc EQUAL 0)
            set(_amiga_pin "")
        endif()

        set(_amiga_candidates "")
        if(_amiga_pin)
            list(APPEND _amiga_candidates "${_amiga_pin}")
        endif()

        # A `current` that runs HERE and is not the pin is an error rather than
        # a lower-ranked candidate.  Ranking around it is what let a build use
        # a toolchain nobody asked for and report success: the only symptom was
        # tools/gen-developer.sh --check calling the committed headers stale.
        if(_amiga_pin AND EXISTS "${_amiga_cache}/current/bin/m68k-amigaos-gcc")
            execute_process(
                COMMAND "${_amiga_cache}/current/bin/m68k-amigaos-gcc" -dumpversion
                RESULT_VARIABLE _amiga_cur_rc
                OUTPUT_VARIABLE _amiga_cur_ver
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET)
            get_filename_component(_amiga_cur_real
                                   "${_amiga_cache}/current" REALPATH)
            get_filename_component(_amiga_pin_real "${_amiga_pin}" REALPATH)
            if(_amiga_cur_rc EQUAL 0 AND NOT _amiga_cur_real STREQUAL _amiga_pin_real)
                if(NOT EXISTS "${_amiga_pin}/bin/m68k-amigaos-gcc")
                    message(FATAL_ERROR
                        "${_amiga_cache}/current is not the toolchain this "
                        "tree pins.\n"
                        "  want ${_amiga_pin}\n"
                        "  got  ${_amiga_cur_real}  (GCC ${_amiga_cur_ver})\n"
                        "Refusing to build with it: the headers, the codegen "
                        "and the library ABI all move.\n"
                        "Run tools/fetch-toolchain.sh, or pass "
                        "-DAMIGA_TOOLCHAIN_ROOT=<path> to say so on purpose.")
                endif()
            endif()
        endif()

        find_program(_amiga_gcc_on_path m68k-amigaos-gcc)
        if(_amiga_gcc_on_path)
            get_filename_component(_amiga_bin "${_amiga_gcc_on_path}" DIRECTORY)
            get_filename_component(_amiga_from_path "${_amiga_bin}" DIRECTORY)
            list(APPEND _amiga_candidates "${_amiga_from_path}")
        endif()

        list(APPEND _amiga_candidates
             "/opt/m68k-amigaos"
             "$ENV{HOME}/amigaos/tools/m68k-amigaos-gcc")

        foreach(_c IN LISTS _amiga_candidates)
            if(EXISTS "${_c}/bin/m68k-amigaos-gcc")
                execute_process(
                    COMMAND "${_c}/bin/m68k-amigaos-gcc" -dumpversion
                    RESULT_VARIABLE _amiga_runs
                    OUTPUT_QUIET ERROR_QUIET)
                if(_amiga_runs EQUAL 0)
                    set(AMIGA_TOOLCHAIN_ROOT "${_c}")
                    break()
                endif()
            endif()
        endforeach()

        if(NOT AMIGA_TOOLCHAIN_ROOT)
            message(FATAL_ERROR
                "No m68k-amigaos cross toolchain that runs on this host.\n"
                "Looked in: ${_amiga_candidates}\n"
                "Fix it with tools/fetch-toolchain.sh, or configure with "
                "-DAMIGA_TOOLCHAIN_ROOT=<path to the dir holding "
                "bin/m68k-amigaos-gcc>.")
        endif()
    endif()
endif()

if(NOT EXISTS "${AMIGA_TOOLCHAIN_ROOT}/bin/m68k-amigaos-gcc")
    message(FATAL_ERROR
        "AMIGA_TOOLCHAIN_ROOT=${AMIGA_TOOLCHAIN_ROOT} has no "
        "bin/m68k-amigaos-gcc.")
endif()

set(AMIGA_TOOLCHAIN_BIN "${AMIGA_TOOLCHAIN_ROOT}/bin")
set(AMIGA_TOOLCHAIN_PREFIX "${AMIGA_TOOLCHAIN_BIN}/m68k-amigaos-")

set(CMAKE_C_COMPILER   "${AMIGA_TOOLCHAIN_PREFIX}gcc")
set(CMAKE_CXX_COMPILER "${AMIGA_TOOLCHAIN_PREFIX}c++")
set(CMAKE_ASM_COMPILER "${AMIGA_TOOLCHAIN_PREFIX}gcc")
set(CMAKE_AR           "${AMIGA_TOOLCHAIN_PREFIX}ar"     CACHE FILEPATH "")
set(CMAKE_RANLIB       "${AMIGA_TOOLCHAIN_PREFIX}ranlib" CACHE FILEPATH "")
set(CMAKE_STRIP        "${AMIGA_TOOLCHAIN_PREFIX}strip"  CACHE FILEPATH "")
set(CMAKE_OBJDUMP      "${AMIGA_TOOLCHAIN_PREFIX}objdump" CACHE FILEPATH "")
set(AMIGA_SIZE         "${AMIGA_TOOLCHAIN_PREFIX}size"   CACHE FILEPATH "")

# The toolchain produces AmigaOS hunk executables, not something the host can run.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(AMIGA_NDK_INCLUDE "${AMIGA_TOOLCHAIN_ROOT}/m68k-amigaos/ndk-include"
    CACHE PATH "NDK include directory (Roadshow bsdsocket + SANA-II headers live here)")

# The NDK headers ship WITH the toolchain, so a root without them is a broken
# or partial install rather than something to work around.  Say so here: the
# alternative is a hundred "exec/types.h: No such file" errors at build time.
if(NOT EXISTS "${AMIGA_NDK_INCLUDE}/exec/types.h")
    message(FATAL_ERROR
        "No NDK headers at ${AMIGA_NDK_INCLUDE}. They come with the toolchain; "
        "if yours keeps them elsewhere, pass -DAMIGA_NDK_INCLUDE=<path>.")
endif()

# The NDK inline headers const-qualify 46 of their register parameters, and
# GCC drops the register binding on a const one whose value is a link-time
# constant: Write(fh, static_buffer, len) then puts the address in d0 and the
# library reads d2, which nobody wrote.  There is no diagnostic and the wrong
# code links, so this is checked here rather than left to be discovered.
# tools/fetch-toolchain.sh repairs a toolchain as it installs it; a cache from
# before that repair existed is still on disk on every machine that had one.
file(READ "${AMIGA_NDK_INCLUDE}/inline/dos.h" _amiga_dos_inline LIMIT 65536)
if(_amiga_dos_inline MATCHES "LP3\\(0x30, LONG, Write[^\n]*const APTR")
    message(FATAL_ERROR
        "${AMIGA_NDK_INCLUDE}/inline/dos.h still declares Write()'s buffer "
        "const APTR, so every call passing a static buffer puts the address "
        "in the wrong register and the file written is whatever d2 held.\n"
        "Repair the toolchain in place:\n"
        "  tools/fix-toolchain-inline-const.py ${AMIGA_TOOLCHAIN_ROOT}\n"
        "tools/fetch-toolchain.sh does this for a tree it installs.")
endif()
unset(_amiga_dos_inline)

# -noixemul is NOT usable with this newlib-based toolchain: it breaks sys/reent.h.
# See docs/RESEARCH.md §5.4.

# ------------------------------------------------------------- target CPU ---
#
# -DAMINETXDUO_CPU=any|68000|68020|68040|68060.  `any` is the default and is
# what ships; the four CPU values build for one part and exist to measure
# against.  docs/RESEARCH.md §45 is where the four entries below come from.
#
# THE FLAGS ARE NOT THE OBVIOUS ONES, because this toolchain ships exactly
# three multilibs, `.` (68000), `libm020` (@mcpu=68020) and `libm060`
# (@mcpu=68060), and the multilib is selected by the canonical -mcpu value:
#
#   68000   -m68020 is the whole difference; the C library is the `.` one.
#   68020   as before.
#   68040   -m68020 -mtune=68040, NOT -m68040.  There is no 68040 multilib, so
#           -m68040 silently selects `.` and links the 68000 C library, code
#           that works but has had every 32-bit multiply and divide turned
#           into a subroutine call.  -m68020 -mtune=68040 keeps libm020 and
#           schedules for the 040, and the 040 implements every 68020
#           instruction, so nothing is lost.  This is also what AmiSSL does:
#           it ships one `68020-40` build and one `68060` build.
#   68060   -mcpu=68060, which is a genuinely different target rather than a
#           tuning choice: the 68060 DROPPED the 64-bit-result forms of MULU.L
#           and DIVU.L, so GCC must not emit them.  They trap to vector 61 and
#           are emulated by 68060.library, which is correct but slow, and it
#           is why the hand-written 68020 assembly must stay off there.
#
# The FPU is deliberately absent from all four.  No -m68881 anywhere: nothing
# in this stack uses floating point (the trust store, the checksums and the
# bignums are all integer), the m68881 multilibs exist only in the 68020 row,
# and a library that requires an FPU would refuse to load on the 68020s and
# 68EC020s that do not have one.  A soft-float build runs everywhere.
#
# `any` is the fifth value and it is not a CPU: it is -m68000 codegen, which
# every 68k runs, plus src/net68k's inner loops assembled once per CPU class
# and chosen from SysBase->AttnFlags at library init.  One binary for the whole
# family.  What it costs is measured, on the A1200 profile, 512 KB loopback,
# six runs an arm: -m68000 throughout is 987.8 ms against the -m68020 build's
# 973.8, and giving the assembly back its 68020 form recovers 6 of those 14 ms.
# The rest is instruction selection spread over the C, and none of it is the
# 32-bit multiply and divide helpers, which no data-path object references
# (src/net68k/n68k_cpu.c).
# THE DEFAULT IS `any`, and the per-CPU values are measurement configurations
# now rather than shipping ones.  The archive has no CPU in it: one build of
# every library, device and command, choosing its own inner loops at init.  Keep
# the specific values -- every figure in this file and in src/net68k was taken
# by building one of them and comparing back to back, and that is the only way
# to take the next one.
set(AMINETXDUO_CPU "any" CACHE STRING "Target CPU: any (every 68k), or 68000, 68020, 68040, 68060 for a measurement build")
set_property(CACHE AMINETXDUO_CPU PROPERTY STRINGS 68000 68020 68040 68060 any)

set(_amiga_cpu_flags_68000 "-m68000")
set(_amiga_cpu_flags_68020 "-m68020")
set(_amiga_cpu_flags_68040 "-m68020;-mtune=68040")
set(_amiga_cpu_flags_68060 "-mcpu=68060")
set(_amiga_cpu_flags_any   "-m68000")

if(NOT DEFINED _amiga_cpu_flags_${AMINETXDUO_CPU})
    message(FATAL_ERROR
        "AMINETXDUO_CPU=${AMINETXDUO_CPU} is not one of 68000, 68020, 68040, "
        "68060, any.  A 68010 runs the 68000 build and a 68030 runs the 68020 "
        "one; there is no separate configuration for either.")
endif()

# AMIGA_ARCH_FLAGS is what the rest of the tree reads, including the two
# CMakeLists that hand it to the assembler by name.  Setting it explicitly
# still works and wins, the probe builds that established §45 were done
# that way, so this only fills it in when nobody has said otherwise.
set(AMIGA_ARCH_FLAGS "${_amiga_cpu_flags_${AMINETXDUO_CPU}}"
    CACHE STRING "Target CPU flags (derived from AMINETXDUO_CPU)")
string(REPLACE ";" " " AMIGA_ARCH_FLAGS_STR "${AMIGA_ARCH_FLAGS}")

set(CMAKE_C_FLAGS_INIT "${AMIGA_ARCH_FLAGS_STR} -fomit-frame-pointer -fno-strict-aliasing")
# -Os, everywhere, and stated rather than inherited.  CMake's Compiler/GNU
# module APPENDS its own "-O3 -DNDEBUG" after CMAKE_C_FLAGS_RELEASE_INIT and
# the last -O on the command line wins, so this line has to name the level it
# wants or it gets -O3 whatever it says.  It said -O2 for a long time and
# produced -O3 builds throughout, which is how every published figure in this
# project came to be measured at a level the file denied.
#
# WHY -Os RATHER THAN -O3 (docs/RESEARCH.md 57)
#
# On a 68000 or an 020 with a 16-bit path to memory, instruction fetch IS the
# bottleneck: there is no cache worth the name on a 68000, the 020's is 256
# bytes, and everything competes for the same slow bus that the data is on.
# Smaller code is faster code far more often than it is on a modern machine,
# and -O3's unrolling and inlining buy speed with exactly the resource that is
# scarcest here.
#
# The hot paths do not depend on the compiler for their speed and never did:
# the checksum and copy primitives, the AES and ChaCha20 kernels are hand
# written 68020 assembly, chosen and measured against C alternatives
# (tests/crypto68k/crypto68k_bulk prints both).  So the optimiser is being
# asked to make the OTHER 95% small, which is what it is good at.  The one
# exception is AMINETXDUO_HOT_O2 (CMakeLists.txt:474): eight profiled NetX Duo
# files build at -O2.
set(CMAKE_C_FLAGS_RELEASE_INIT "-Os -DNDEBUG")
set(CMAKE_C_FLAGS_DEBUG_INIT "-O1 -g -DAMINETXDUO_DEBUG=1")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM BEFORE)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
