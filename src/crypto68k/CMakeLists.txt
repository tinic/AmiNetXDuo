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

# THE ASSEMBLY IS 68020-ONLY, and on two of the four targets it is not merely
# unhelpful but unassemblable or actively wrong:
#
#   68000   mulu.l, divu.l, extb.l and the scaled index (a1,d6.w*4) do not
#           exist.  The assembler rejects the files outright.
#   68060   they assemble, but MULU.L's 32x32 -> 64 form was dropped from the
#           68060 and traps to vector 61 on every limb multiply.  It is
#           emulated by 68060.library, so it works; it works at the cost of an
#           exception per multiply, which is far worse than the portable C's
#           four mulu.w.  AmiSSL reached the same conclusion and ships its
#           68060 build without its own m68k bignum assembly.
#
# Hence the default is a function of the CPU rather than of cross-compiling
# alone.  It can still be forced either way by hand -- turning it off is the
# supported way to bisect a suspected assembly bug -- but nobody has to know
# the above to get a working build.  docs/RESEARCH.md §45.
if(CMAKE_CROSSCOMPILING AND AMINETXDUO_CPU MATCHES "^(68020|68040)$")
    set(_crypto68k_asm_default ON)
else()
    set(_crypto68k_asm_default OFF)
endif()

option(AMINETXDUO_CRYPTO68K_ASM
       "Use the hand-written 68020 limb primitives in crypto68k"
       ${_crypto68k_asm_default})

if(AMINETXDUO_CRYPTO68K_ASM AND NOT _crypto68k_asm_default AND CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR
        "AMINETXDUO_CRYPTO68K_ASM=ON with AMINETXDUO_CPU=${AMINETXDUO_CPU}. "
        "The hand-written primitives need 68020 instructions: a 68000 cannot "
        "assemble them and a 68060 traps on every MULU.L. Leave it off.")
endif()

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
    c68k_sha256.c
    # ChaCha20-Poly1305, the AEAD record path.  Both halves have a 68020
    # counterpart -- c68k_chacha20.S and c68k_poly1305.S -- and in both cases
    # the reason is register allocation rather than instruction selection: the
    # cipher is sixteen state words against eight data registers (57, 58), and
    # the authenticator is fourteen live values and a 32x32 -> 64 product that
    # C can only reach through `unsigned long long`, which at -Os costs it a
    # frame round trip per product.  The C stays compiled in the assembly build
    # as the reference the tests compare against.
    c68k_chacha20.c
    c68k_poly1305.c
    # X25519 and Ed25519.  No assembly counterpart: the win is the field
    # representation (eight 32-bit limbs instead of TweetNaCl's sixteen 16-bit
    # limbs in an i64[16]), and 18.4 established that this GCC does not leave
    # instruction selection on the table for a loop like this.  See
    # docs/RESEARCH.md 35.
    c68k_25519.c)

if(AMINETXDUO_CRYPTO68K_ASM)
    list(APPEND CRYPTO68K_SOURCES
         c68k_prim.S c68k_p256.S c68k_aes.S c68k_chacha20.S c68k_poly1305.S)
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
        c68k_prim.S c68k_p256.S c68k_aes.S c68k_chacha20.S c68k_poly1305.S
        PROPERTIES
        COMPILE_DEFINITIONS C68K_ASM=1
        COMPILE_OPTIONS "${AMIGA_ARCH_FLAGS}")
endif()

message(STATUS "  crypto68k asm . ${AMINETXDUO_CRYPTO68K_ASM}")
