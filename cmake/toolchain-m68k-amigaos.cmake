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
#   1. -DAMIGA_TOOLCHAIN_ROOT / $AMIGA_TOOLCHAIN_ROOT   -- explicit
#   2. the tools/fetch-toolchain.sh cache               -- what CI uses
#   3. m68k-amigaos-gcc on $PATH                        -- container, module
#   4. /opt/m68k-amigaos                                -- crosstools layout
#   5. $HOME/amigaos/tools/m68k-amigaos-gcc             -- local default
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

        set(_amiga_candidates "${_amiga_cache}/current")

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

# -noixemul is NOT usable with this newlib-based toolchain: it breaks sys/reent.h.
# See docs/RESEARCH.md §5.4.
set(AMIGA_ARCH_FLAGS "-m68020" CACHE STRING "Target CPU flags (68020 floor, see docs/RESEARCH.md §9)")

set(CMAKE_C_FLAGS_INIT "${AMIGA_ARCH_FLAGS} -fomit-frame-pointer -fno-strict-aliasing")
# -O3, stated rather than inherited.  This used to say -O2 and had never once
# produced an -O2 build: CMake's Compiler/GNU module APPENDS its own
# "-O3 -DNDEBUG" after CMAKE_C_FLAGS_RELEASE_INIT, and the last -O on the
# command line wins.  Every figure this project has published -- the checksum
# and copy primitives, the crypto, the throughput numbers -- was measured on an
# -O3 build while the file claimed -O2.
#
# Left at -O3 deliberately rather than "fixed" to -O2: -O3 is what everything
# was measured at, and forcing -O2 would invalidate those numbers to honour a
# comment.  src/tools appends -Os after this, which is why the commands are the
# one thing that really is built for size.
set(CMAKE_C_FLAGS_RELEASE_INIT "-O3 -DNDEBUG")
set(CMAKE_C_FLAGS_DEBUG_INIT "-O1 -g -DAMINETXDUO_DEBUG=1")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM BEFORE)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
