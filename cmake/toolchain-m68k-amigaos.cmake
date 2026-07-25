# CMake toolchain for m68k-amigaos-gcc (AmigaPorts / newlib).
#
# Usage:
#   cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-m68k-amigaos.cmake
#
# Override the toolchain root with -DAMIGA_TOOLCHAIN_ROOT=<path> if it is not the
# default location used by the local amigaos/ checkout.

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR m68k)

if(NOT AMIGA_TOOLCHAIN_ROOT)
    if(DEFINED ENV{AMIGA_TOOLCHAIN_ROOT})
        set(AMIGA_TOOLCHAIN_ROOT "$ENV{AMIGA_TOOLCHAIN_ROOT}")
    else()
        set(AMIGA_TOOLCHAIN_ROOT "$ENV{HOME}/amigaos/tools/m68k-amigaos-gcc")
    endif()
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

# -noixemul is NOT usable with this newlib-based toolchain: it breaks sys/reent.h.
# See docs/RESEARCH.md §5.4.
set(AMIGA_ARCH_FLAGS "-m68020" CACHE STRING "Target CPU flags (68020 floor, see docs/RESEARCH.md §9)")

set(CMAKE_C_FLAGS_INIT "${AMIGA_ARCH_FLAGS} -fomit-frame-pointer -fno-strict-aliasing")
set(CMAKE_C_FLAGS_RELEASE_INIT "-O2 -DNDEBUG")
set(CMAKE_C_FLAGS_DEBUG_INIT "-O1 -g -DAMINETXDUO_DEBUG=1")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM BEFORE)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
