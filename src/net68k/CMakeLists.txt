# net68k, 68k data-path primitives for NetX Duo.
#
# AMINETXDUO_NET68K_ASM selects the hand-written 68020 inner loops.  It
# defaults ON when cross-compiling and OFF otherwise, exactly like
# AMINETXDUO_CRYPTO68K_ASM, so a host build still compiles the module in
# portable C, which is what the host tier of the differential test runs --
# and nothing silently picks up assembly that cannot run.  Turning it off is
# the supported way to bisect a suspected assembly bug: the C fallback is the
# same carry chain, longword for longword.
#
# THE TWO SWITCHES HAVE COME APART, and this is now the one that stays on for
# every CPU.  It used to be paired with AMINETXDUO_CRYPTO68K_ASM as a single
# "hand-written 68020 code" decision, which was fine while 68020 was the only
# target.  Adding the 68000 and 68060 builds made the difference matter: both
# files here use nothing but add.l, addx.l, movem.l and dbf, all of which are
# in the original 68000 instruction set and none of which the 68060 dropped,
# so both assemble and run on all four targets, checked, not assumed.
# crypto68k's do not, and it is gated on the CPU accordingly.
#
# This is the good outcome for the 68000 in particular: the checksum and copy
# inner loops are the data path, and they keep their assembly on the slowest
# machine, which is the one that needs it most.  docs/RESEARCH.md §45.
#
# SPDX-License-Identifier: MIT

include(CMakeDependentOption)

option(AMINETXDUO_NET68K_ASM
       "Use the hand-written 68020 inner loops in net68k"
       ${CMAKE_CROSSCOMPILING})

# Replacing the C library's memcpy() is a separate decision from replacing
# NetX Duo's checksum, so it is a separate switch.  Default follows the
# measurement in docs/RESEARCH.md; build both and compare before changing it.
#
# CROSS BUILDS ONLY, and not merely because the movem.l loop is m68k.  A host
# build links a host libc, and defining memcpy() there would replace the C
# library's own, in a test binary, for the whole process.  It also does not
# compile: on macOS <string.h> may define memcpy as a macro, so the definition
# is a syntax error rather than an override.  Neither is a thing to work
# around; the hook has no business existing off target.
cmake_dependent_option(AMINETXDUO_NET68K_MEMCPY
       "Resolve memcpy() to net68k's movem.l copy" ON
       "CMAKE_CROSSCOMPILING" OFF)

set(NET68K_SOURCES
    n68k_checksum.c
    n68k_checksum_hook.c
    n68k_copy.c)

if(AMINETXDUO_NET68K_MEMCPY)
    list(APPEND NET68K_SOURCES n68k_memcpy_hook.c)
endif()

if(AMINETXDUO_NET68K_ASM)
    list(APPEND NET68K_SOURCES n68k_checksum.S n68k_copy.S)
endif()

add_library(aminetxduo_net68k STATIC ${NET68K_SOURCES})

target_include_directories(aminetxduo_net68k PUBLIC
    ${AMINETXDUO_CORE_INCLUDES}
    "${CMAKE_CURRENT_SOURCE_DIR}")

# Headers only, deliberately no target_link_libraries(... netxduo): the
# top-level CMakeLists makes netxduo depend on THIS, and a static-library
# cycle would be pointless here, nothing in net68k calls back into NetX Duo.

if(AMINETXDUO_NET68K_ASM)
    # The .S is #ifdef'd end to end, so it needs the define as well, an
    # accidental host build then assembles to nothing rather than failing.
    # It also needs the arch flag explicitly: CMAKE_C_FLAGS_INIT in the
    # toolchain file does not reach the assembler.  Same arrangement as
    # src/crypto68k/CMakeLists.txt, and for the same reasons.
    target_compile_definitions(aminetxduo_net68k PRIVATE AMINETXDUO_NET68K_ASM=1)
    set_source_files_properties(n68k_checksum.S n68k_copy.S PROPERTIES
        COMPILE_DEFINITIONS AMINETXDUO_NET68K_ASM=1
        COMPILE_OPTIONS "${AMIGA_ARCH_FLAGS}")
endif()

message(STATUS "  net68k asm .... ${AMINETXDUO_NET68K_ASM}")
