# net68k -- 68k data-path primitives for NetX Duo.
#
# AMINETXDUO_NET68K_ASM selects the hand-written 68020 inner loops.  It
# defaults ON when cross-compiling and OFF otherwise, exactly like
# AMINETXDUO_CRYPTO68K_ASM, so a host build still compiles the module in
# portable C -- which is what the host tier of the differential test runs --
# and nothing silently picks up assembly that cannot run.  Turning it off is
# the supported way to bisect a suspected assembly bug: the C fallback is the
# same carry chain, longword for longword.
#
# 68060: keep this OFF, for the same reason AMINETXDUO_CRYPTO68K_ASM must be
# off there.  Nothing in n68k_checksum.S itself is 68060-hostile, but the two
# switches are meant to be read as one "hand-written 68020 code" decision and
# crypto68k's MULU.L 32x32 traps to vector 61 on a 68060.
#
# SPDX-License-Identifier: MIT

option(AMINETXDUO_NET68K_ASM
       "Use the hand-written 68020 inner loops in net68k"
       ${CMAKE_CROSSCOMPILING})

set(NET68K_SOURCES
    n68k_checksum.c
    n68k_checksum_hook.c)

if(AMINETXDUO_NET68K_ASM)
    list(APPEND NET68K_SOURCES n68k_checksum.S)
endif()

add_library(aminetxduo_net68k STATIC ${NET68K_SOURCES})

target_include_directories(aminetxduo_net68k PUBLIC
    ${AMINETXDUO_CORE_INCLUDES}
    "${CMAKE_CURRENT_SOURCE_DIR}")

# Headers only, deliberately no target_link_libraries(... netxduo): the
# top-level CMakeLists makes netxduo depend on THIS, and a static-library
# cycle would be pointless here -- nothing in net68k calls back into NetX Duo.

if(AMINETXDUO_NET68K_ASM)
    # The .S is #ifdef'd end to end, so it needs the define as well -- an
    # accidental host build then assembles to nothing rather than failing.
    # It also needs the arch flag explicitly: CMAKE_C_FLAGS_INIT in the
    # toolchain file does not reach the assembler.  Same arrangement as
    # src/crypto68k/CMakeLists.txt, and for the same reasons.
    target_compile_definitions(aminetxduo_net68k PRIVATE AMINETXDUO_NET68K_ASM=1)
    set_source_files_properties(n68k_checksum.S PROPERTIES
        COMPILE_DEFINITIONS AMINETXDUO_NET68K_ASM=1
        COMPILE_OPTIONS "${AMIGA_ARCH_FLAGS}")
endif()

message(STATUS "  net68k asm .... ${AMINETXDUO_NET68K_ASM}")
