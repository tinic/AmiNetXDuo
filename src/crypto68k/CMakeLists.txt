# crypto68k -- 68020 multi-precision arithmetic for nx_crypto.
#
#   cmake -S . -B build/cm -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-m68k-amigaos.cmake \
#         -DCMAKE_BUILD_TYPE=Release
#   cmake --build build/cm --parallel --target crypto68k_test
#
# AMINETXDUO_CRYPTO68K_ASM selects the hand-written 68020 limb primitives.  It
# defaults ON when cross-compiling and OFF otherwise, so a host build still
# compiles the module (portable C only) and nothing silently picks up assembly
# that cannot run.  Turning it off is the supported way to bisect a suspected
# assembly bug: the C fallback is the same algorithm, limb for limb.
#
# The module needs the vendored huge-number code for exactly one thing -- the
# radix^2 mod m long division in the exponentiation setup -- and the test needs
# it as the reference to check every result against.  src/tls/ owns the full
# nx_crypto target but only builds it when AMINETXDUO_TLS is ON, and this must
# stand alone, so the two files that are actually required are compiled here
# into a private archive.  Compiling vendored sources is not modifying them.
#
# SPDX-License-Identifier: MIT

option(AMINETXDUO_CRYPTO68K_ASM
       "Use the hand-written 68020 limb primitives in crypto68k"
       ${CMAKE_CROSSCOMPILING})

set(NX_CRYPTO_ROOT "${AMINETXDUO_NETXDUO}/crypto_libraries")

set(CRYPTO68K_INCLUDES
    ${AMINETXDUO_CORE_INCLUDES}
    "${NX_CRYPTO_ROOT}/inc"
    "${CMAKE_CURRENT_SOURCE_DIR}")

# --------------------------------------------------- vendored reference ---

# nx_crypto_huge_number.c divides 64-bit values, and this toolchain ships a
# zero-byte libgcc.a.  src/tls/tls_udivdi3.c already solves that, but src/tls
# is only configured when AMINETXDUO_TLS is ON -- so reuse its target when it
# exists and build our own copy of the same file when it does not.  Never both
# in one link: the definitions are identical and would collide.
if(TARGET aminetxduo_m68k_rt)
    set(CRYPTO68K_RT aminetxduo_m68k_rt)
elseif(CMAKE_CROSSCOMPILING)
    add_library(crypto68k_rt STATIC "${CMAKE_CURRENT_SOURCE_DIR}/../tls/tls_udivdi3.c")
    set(CRYPTO68K_RT crypto68k_rt)
else()
    set(CRYPTO68K_RT "")
endif()

add_library(crypto68k_ref STATIC
    "${NX_CRYPTO_ROOT}/src/nx_crypto_huge_number.c"
    "${NX_CRYPTO_ROOT}/src/nx_crypto_initialize.c")
target_include_directories(crypto68k_ref PUBLIC ${CRYPTO68K_INCLUDES})
target_link_libraries(crypto68k_ref PUBLIC netxduo)
if(CRYPTO68K_RT)
    target_link_libraries(crypto68k_ref PUBLIC ${CRYPTO68K_RT})
endif()

# ------------------------------------------------------------ the module ---

set(CRYPTO68K_SOURCES
    c68k_prim.c
    c68k_mont.c
    c68k_powm.c
    c68k_crt.c)

if(AMINETXDUO_CRYPTO68K_ASM)
    list(APPEND CRYPTO68K_SOURCES c68k_prim.S)
endif()

add_library(crypto68k STATIC ${CRYPTO68K_SOURCES})
target_include_directories(crypto68k PUBLIC ${CRYPTO68K_INCLUDES})
target_link_libraries(crypto68k PUBLIC crypto68k_ref)

if(AMINETXDUO_CRYPTO68K_ASM)
    # The .S needs the define too -- it is #ifdef'd end to end so that an
    # accidental host build assembles to nothing instead of failing.
    #
    # It also needs the arch flag explicitly.  CMAKE_C_FLAGS_INIT in the
    # toolchain file does not reach the assembler, and without -m68020 the
    # assembler is a plain 68000 and rejects MULU.L outright:
    #   "invalid instruction for this architecture ... statement
    #    `mulu.l d2,d1:d3' ignored"
    # which is a loud failure, but only because the instruction does not exist
    # on a 68000 at all.  Set here rather than in the shared toolchain file so
    # nothing else in the tree changes behaviour.
    target_compile_definitions(crypto68k PRIVATE C68K_ASM=1)
    set_source_files_properties(c68k_prim.S PROPERTIES
        COMPILE_DEFINITIONS C68K_ASM=1
        COMPILE_OPTIONS "${AMIGA_ARCH_FLAGS}")
endif()

message(STATUS "  crypto68k asm . ${AMINETXDUO_CRYPTO68K_ASM}")
