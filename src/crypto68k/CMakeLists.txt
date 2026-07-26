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
# zero-byte libgcc.a.  src/common/ami_udivdi3.c is the single copy of the
# helpers for the whole tree; src/common is configured unconditionally and
# before this directory, so the target is simply there on a cross build.  On a
# host build libgcc supplies the real ones.
if(TARGET aminetxduo_m68k_rt)
    set(CRYPTO68K_RT aminetxduo_m68k_rt)
else()
    set(CRYPTO68K_RT "")
endif()

# When AMINETXDUO_TLS is ON, src/tls has ALREADY built the whole of
# crypto_libraries/src as `nx_crypto` -- it is configured first, see the top
# level CMakeLists.txt -- and compiling these ten files a second time here puts
# two definitions of every huge-number and EC symbol on the link line.  Reuse
# that target instead; it is a superset of this list.  The standalone archive is
# only for the AMINETXDUO_TLS=OFF build, where crypto68k and its tests still
# have to stand on their own.
if(TARGET nx_crypto)
    add_library(crypto68k_ref INTERFACE)
    target_include_directories(crypto68k_ref INTERFACE ${CRYPTO68K_INCLUDES})
    target_link_libraries(crypto68k_ref INTERFACE nx_crypto)
else()
    add_library(crypto68k_ref STATIC
        "${NX_CRYPTO_ROOT}/src/nx_crypto_huge_number.c"
        "${NX_CRYPTO_ROOT}/src/nx_crypto_initialize.c"
        # The bulk path's reference implementations.  tests/crypto68k checks
        # every AES and SHA-256 variant against these before it times one, so
        # they have to be present in the AMINETXDUO_TLS=OFF build too.
        "${NX_CRYPTO_ROOT}/src/nx_crypto_aes.c"
        "${NX_CRYPTO_ROOT}/src/nx_crypto_cbc.c"
        "${NX_CRYPTO_ROOT}/src/nx_crypto_sha2.c"
        "${NX_CRYPTO_ROOT}/src/nx_crypto_hmac.c"
        # nx_crypto_aes.c's method dispatch reaches CCM, GCM and XCBC even
        # when nothing calls them, so a standalone archive that omits them
        # does not link.  Compiling vendored sources is not modifying them.
        "${NX_CRYPTO_ROOT}/src/nx_crypto_ccm.c"
        "${NX_CRYPTO_ROOT}/src/nx_crypto_gcm.c"
        "${NX_CRYPTO_ROOT}/src/nx_crypto_xcbc_mac.c"
        "${NX_CRYPTO_ROOT}/src/nx_crypto_ctr.c"
        "${NX_CRYPTO_ROOT}/src/nx_crypto_ec.c"
        "${NX_CRYPTO_ROOT}/src/nx_crypto_ecdh.c"
        "${NX_CRYPTO_ROOT}/src/nx_crypto_ecdsa.c"
        "${NX_CRYPTO_ROOT}/src/nx_crypto_ec_secp192r1_fixed_points.c"
        "${NX_CRYPTO_ROOT}/src/nx_crypto_ec_secp224r1_fixed_points.c"
        "${NX_CRYPTO_ROOT}/src/nx_crypto_ec_secp256r1_fixed_points.c"
        "${NX_CRYPTO_ROOT}/src/nx_crypto_ec_secp384r1_fixed_points.c"
        "${NX_CRYPTO_ROOT}/src/nx_crypto_ec_secp521r1_fixed_points.c")
    target_include_directories(crypto68k_ref PUBLIC ${CRYPTO68K_INCLUDES})
    target_link_libraries(crypto68k_ref PUBLIC netxduo)
    if(CRYPTO68K_RT)
        target_link_libraries(crypto68k_ref PUBLIC ${CRYPTO68K_RT})
    endif()
endif()

# ------------------------------------------------------------ the module ---

set(CRYPTO68K_SOURCES
    c68k_prim.c
    c68k_div.c
    c68k_mont.c
    c68k_powm.c
    c68k_crt.c
    c68k_p256.c
    c68k_p256_table.c
    c68k_aes.c
    c68k_sha256.c)

if(AMINETXDUO_CRYPTO68K_ASM)
    list(APPEND CRYPTO68K_SOURCES
         c68k_prim.S c68k_p256.S c68k_aes.S)
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
    set_source_files_properties(
        c68k_prim.S c68k_p256.S c68k_aes.S PROPERTIES
        COMPILE_DEFINITIONS C68K_ASM=1
        COMPILE_OPTIONS "${AMIGA_ARCH_FLAGS}")
endif()

message(STATUS "  crypto68k asm . ${AMINETXDUO_CRYPTO68K_ASM}")
