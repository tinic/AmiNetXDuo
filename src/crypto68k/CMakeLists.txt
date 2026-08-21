# crypto68k, 68020 multi-precision arithmetic for nx_crypto.
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
# The module needs the vendored huge-number code for exactly one thing, the
# radix^2 mod m long division in the exponentiation setup, and the test needs
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
# alone.  It can still be forced either way by hand, turning it off is the
# supported way to bisect a suspected assembly bug, but nobody has to know
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

# The 68000 and the 68060 get ONE of them back.  Only two instructions in c68k_prim.S are
# missing from that part, MULU.L's 32x32 -> 64 form and DIVU.L, but the file is
# all or nothing, so the 68060 takes the portable C for the whole module -- and
# then spends 64% of a TLS 1.3 transfer in c68k_addmul_1_c alone.
# c68k_prim_mulw.S is that one function rewritten in MULU.W, which both parts
# do implement -- the 68060 because only the 64-bit form was dropped, the
# 68000 because MULU.W is the only multiply it ever had.  It is a separate option because it is a separate file with a
# separate CPU rule, and because the two cannot both be on: they define the same
# symbol.
# `any` takes it for the reason both of those do, and it is the only assembly
# here a CPU-independent binary can carry: MULU.W is in the original 68000 set
# and the 68060 kept it, so one build of this file runs on the whole family.
if(CMAKE_CROSSCOMPILING AND (AMINETXDUO_CPU STREQUAL "68060" OR
                            AMINETXDUO_CPU STREQUAL "68000" OR
                            AMINETXDUO_CPU STREQUAL "any"))
    set(_crypto68k_asm_mulw_default ON)
else()
    set(_crypto68k_asm_mulw_default OFF)
endif()

option(AMINETXDUO_CRYPTO68K_ASM_MULW
       "Use the hand-written 68060 limb multiply-accumulate in crypto68k"
       ${_crypto68k_asm_mulw_default})

if(AMINETXDUO_CRYPTO68K_ASM_MULW AND AMINETXDUO_CRYPTO68K_ASM)
    message(FATAL_ERROR
        "AMINETXDUO_CRYPTO68K_ASM and AMINETXDUO_CRYPTO68K_ASM_MULW are both ON. "
        "Both define c68k_addmul_1. Pick one.")
endif()

if(AMINETXDUO_CRYPTO68K_ASM_MULW AND NOT CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR
        "AMINETXDUO_CRYPTO68K_ASM_MULW=ON on a host build. The host cannot "
        "assemble m68k.")
endif()

set(NX_CRYPTO_ROOT "${AMINETXDUO_NETXDUO}/crypto_libraries")

set(CRYPTO68K_INCLUDES
    ${AMINETXDUO_CORE_INCLUDES}
    "${NX_CRYPTO_ROOT}/inc"
    "${CMAKE_CURRENT_SOURCE_DIR}")

# --------------------------------------------------- vendored reference ---

# nx_crypto_huge_number.c divides 64-bit values.  src/common/ami_udivdi3.c is
# the single CPU-dispatched copy of the helpers for the whole tree; src/common
# is configured unconditionally and before this directory, so the target is
# simply there on a cross build.  On a host build libgcc supplies them.
if(TARGET aminetxduo_m68k_rt)
    set(CRYPTO68K_RT aminetxduo_m68k_rt)
else()
    set(CRYPTO68K_RT "")
endif()

# When AMINETXDUO_TLS is ON, src/tls has ALREADY built the whole of
# crypto_libraries/src as `nx_crypto`, it is configured first, see the top
# level CMakeLists.txt, and compiling these ten files a second time here puts
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
    # counterpart, c68k_chacha20.S and c68k_poly1305.S, and in both cases
    # the reason is register allocation rather than instruction selection: the
    # cipher is sixteen state words against eight data registers (57, 58), and
    # the authenticator is fourteen live values and a 32x32 -> 64 product that
    # C can only reach through `unsigned long long`, which at -Os costs it a
    # frame round trip per product.  The C stays compiled in the assembly build
    # as the reference the tests compare against.
    c68k_chacha20.c
    c68k_poly1305.c
    # X25519 and Ed25519.  The field representation was the first win (eight
    # 32-bit limbs instead of TweetNaCl's sixteen 16-bit limbs in an i64[16]);
    # docs/RESEARCH.md 35.4 then deferred assembly, on 18.4's grounds, with the
    # condition that it "should be asked against a measurement of this, not
    # instead of one".
    #
    # That measurement exists now.  Profiling one real ssh handshake from
    # inside the guest put 45.8% of the WHOLE CONNECTION in fe_mul and 19.7%
    # in fe_sqr on a 13.9 MHz 68020, so c68k_25519.S is those four routines,
    # and it beats the C for three reasons of which only one is instruction
    # selection: t[16] was zeroed with memset (4.3% of the connection under
    # its own name, more than sha256 and chacha20 together), the accumulator
    # made a round trip through an array subscript every limb, and the 38-fold
    # was a second pass over sixteen words.  The C stays compiled as the
    # reference the tests compare against, exactly as for chacha20 and
    # poly1305.
    c68k_25519.c)

# c68k_25519.S is in BOTH lists and is one file: it carries the MULU.L field
# multiply for the 68020/68040 and the four-MULU.W one for the 68000/68060,
# picked by the same define that selects the rest of the module.  That is why
# it is not a third option -- unlike c68k_prim.S and c68k_prim_mulw.S, which
# are two files defining the same symbol and cannot both be built, these are
# two halves of one file and exactly one is ever assembled.
list(APPEND CRYPTO68K_SOURCES c68k_cpu.c)

if(AMINETXDUO_CRYPTO68K_ASM)
    list(APPEND CRYPTO68K_SOURCES
         c68k_prim.S c68k_p256.S c68k_aes.S c68k_chacha20.S c68k_poly1305.S
         c68k_25519.S)
endif()

if(AMINETXDUO_CRYPTO68K_ASM_MULW)
    list(APPEND CRYPTO68K_SOURCES c68k_prim_mulw.S c68k_25519.S)
endif()

# ------------------------------------------------ one binary for every CPU ---
#
# AMINETXDUO_CPU=any takes both halves of the module and chooses at run time.
# What that needs is narrower than it looks, because most of the assembly here
# is legal on every 68k and only two routines are not:
#
#   c68k_chacha20.S          assembles at -m68000, checked: 0 illegal
#                            instructions.  So the 68000 and the 68060 get the
#                            ChaCha20 kernel they have never had, and the
#                            default TLS cipher is ChaCha20-Poly1305 (54).
#   c68k_prim.S              add, sub, cmp and add_carry are original 68000
#                            code and are called by name everywhere.  Only
#                            addmul_1 (MULU.L 32x32 -> 64) and div_2by1
#                            (DIVU.L 64/32) are 68020-to-68040, and those two
#                            are the ones c68k_cpu.c dispatches.
#   c68k_prim_mulw.S         the MULU.W addmul_1, legal everywhere, and the
#                            answer for both the 68000 and the 68060.
#   c68k_poly1305.S          a 32x32 -> 64 product per block: dispatched.
#
# The file is assembled for its own needs rather than for the machine: an
# object may hold an instruction this CPU lacks as long as nothing calls the
# routine that uses it.  Hence -m68020 on c68k_prim.S in a -m68000 binary.
#
#   c68k_aes.S               the 68020 scaled index and nothing else, which
#                            the 68060 kept, so it is assembled -m68020 and
#                            selected on AFF_68020 alone.  c68k_aes_variant was
#                            already a run-time choice; this only sets its
#                            default (c68k_cpu.c).
#   c68k_25519.S             two halves of one file, so two objects, exactly
#                            as src/net68k's variants are.
#
# c68k_p256.S is NOT here yet: one instruction in a per-limb macro (extb.l) has
# to be substituted before a 68000 can run it, which is its own commit.
set(CRYPTO68K_MV OFF)
if(CMAKE_CROSSCOMPILING AND AMINETXDUO_CPU STREQUAL "any")
    set(CRYPTO68K_MV ON)
    list(APPEND CRYPTO68K_SOURCES
         c68k_prim.S c68k_chacha20.S c68k_poly1305.S c68k_prim_mulw.S
         c68k_aes.S c68k_dispatch.S)

    # c68k_25519.S is one file holding both halves, picked by a define, so it
    # becomes two objects the way src/net68k's variants do -- and for the same
    # two reasons, a translation unit of its own and a file name of its own.
    # This is also exactly what one ssh binary needs: clients/dropbear compiles
    # c68k_25519.c itself and picks the half with the same define today.
    foreach(mv mulu:C68K_ASM:-m68020:c68k_25519.S
               mulw:C68K_ASM_MULW:-m68000:c68k_25519.S
               mv20:C68K_P256_EXTB:-m68020:c68k_p256.S
               mv0:C68K_ASM:-m68000:c68k_p256.S)
        string(REPLACE ":" ";" _parts "${mv}")
        list(GET _parts 0 _sfx)
        list(GET _parts 1 _def)
        list(GET _parts 2 _arch)
        list(GET _parts 3 _src)
        string(REPLACE ".S" "" _stem "${_src}")
        set(C68K_MV_DEFINE "${_def}")
        set(C68K_MV_SUFFIX "_${_sfx}")
        set(C68K_MV_SRC "${_src}")
        set(_gen "${CMAKE_CURRENT_BINARY_DIR}/${_stem}_${_sfx}.S")
        configure_file(c68k_mv.S.in "${_gen}" @ONLY)
        list(APPEND CRYPTO68K_SOURCES "${_gen}")
        set_source_files_properties("${_gen}" PROPERTIES
            COMPILE_DEFINITIONS "C68K_MV=1;C68K_ASM=1"
            COMPILE_OPTIONS "${_arch}"
            INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}")
    endforeach()
    unset(C68K_MV_DEFINE)
    unset(C68K_MV_SUFFIX)
    unset(C68K_MV_SRC)
endif()

# The full-link regression for the LTO failure which originally looked like a
# P-256 miscompile.  The arithmetic was correct; WPA deleted compiler-runtime
# helpers whose calls appear only during LTRANS.  src/common/ami_udivdi3.c now
# keeps those entry points.  This probe runs field known answers and both
# scalar-multiply paths from the real tls.library link in about fifteen seconds.
option(AMINETXDUO_CRYPTO68K_LTO_PROBE
       "Run the P-256 LTO regression check while loading tls.library"
       OFF)

if(AMINETXDUO_CRYPTO68K_LTO_PROBE)
    if(NOT CMAKE_CROSSCOMPILING OR NOT AMINETXDUO_TLS OR NOT AMINETXDUO_LTO)
        message(FATAL_ERROR
            "AMINETXDUO_CRYPTO68K_LTO_PROBE needs a cross build with both "
            "AMINETXDUO_TLS=ON and AMINETXDUO_LTO=ON.")
    endif()
endif()

# -fno-lto ON EVERY C FILE HERE, AGAIN, FOR A SECOND AND DIFFERENT DEFECT.
#
# The first one was the late compiler-libcall reachability bug in
# src/common/ami_udivdi3.c, and that IS fixed: `used' holds the helpers, and
# the release gate confirms each of the five survivors sits at its own address
# with a real size in its own .text.<name> section -- __muldi3 is 100 bytes at
# 0x2a79c with a real body, and __divdi3/__moddi3 are properly --gc-sections
# discarded. That check passes. This one is not that.
#
# With crypto68k under -flto at 727b5677, tls.library completes ZERO TLS 1.3
# handshakes, three gate runs for three, and `fetch https://' returns rc=10
# against www.gnu.org, www.amiga-news.de and www.pouet.net from a real
# installed Workbench 3.1. Three controls in one checkout, one peer, twenty
# minutes apart: -DAMINETXDUO_LTO=OFF passes 2/2; the same tree with only
# tls.library replaced by the crypto68k-excluded build passes 2/2; the
# shipping default fails 3/3.
#
# THE SIGNATURE CHANGED, which is why this is a second thing and not a
# regression of the first. The old failure was BAD_ECPOINT -- a well-formed
# key share carrying a point that is not on the curve. This one is
# UNEXPECTED_EOF_WHILE_READING at 0.5s: the far end sees the connection go
# away mid-handshake. Nothing is off-curve because nothing gets that far.
#
# src/crypto68k/CMakeLists.txt:288 has the instrument for it:
# -DAMINETXDUO_CRYPTO68K_LTO_PROBE=ON runs the P-256 known answers at
# tls.library load, with no peer and no network, and reports through
# lib_Revision. Use that rather than the handshake gate -- the last time this
# was bisected by rebuild-and-handshake it cost hours per arm.
foreach(_c68k_src IN LISTS CRYPTO68K_SOURCES)
    if(_c68k_src MATCHES "\\.c$")
        set_source_files_properties("${_c68k_src}"
            PROPERTIES COMPILE_OPTIONS -fno-lto)
    endif()
endforeach()

add_library(crypto68k STATIC ${CRYPTO68K_SOURCES})
target_include_directories(crypto68k PUBLIC ${CRYPTO68K_INCLUDES})
target_link_libraries(crypto68k PUBLIC crypto68k_ref)

if(AMINETXDUO_CRYPTO68K_LTO_PROBE)
    target_compile_definitions(crypto68k PRIVATE
        AMINETXDUO_CRYPTO68K_LTO_PROBE=1)
endif()

if(AMINETXDUO_CRYPTO68K_ASM)
    # The .S needs the define too, it is #ifdef'd end to end so that an
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
        c68k_25519.S
        PROPERTIES
        COMPILE_DEFINITIONS C68K_ASM=1
        COMPILE_OPTIONS "${AMIGA_ARCH_FLAGS}")
endif()

if(AMINETXDUO_CRYPTO68K_ASM_MULW)
    # Same two reasons as above: the .S is #ifdef'd end to end, and the
    # assembler does not see CMAKE_C_FLAGS_INIT, so without the arch flag it is
    # a plain 68000 and rejects nothing here but assembles for the wrong part.
    target_compile_definitions(crypto68k PRIVATE C68K_ASM_MULW=1)
    set_source_files_properties(c68k_prim_mulw.S c68k_25519.S PROPERTIES
        COMPILE_DEFINITIONS C68K_ASM_MULW=1
        COMPILE_OPTIONS "${AMIGA_ARCH_FLAGS}")
endif()

if(CRYPTO68K_MV)
    # C68K_ASM tells the .c files the assembly exists and to declare it;
    # C68K_MV tells them the choice is a vector rather than a name.  PUBLIC on
    # both: crypto68k.h resolves C68K_ADDMUL_1 and C68K_DIV_2BY1 on C68K_MV,
    # and src/tls and tests/crypto68k are callers that must agree with this
    # directory about which one those are.
    # C68K_ASM says the module has assembly, C68K_MV says the choice is a
    # vector rather than a name, and the per-file gates below say WHICH files
    # are here (c68k_variant.h).  PUBLIC on all of them: crypto68k.h resolves
    # C68K_ADDMUL_1 and C68K_DIV_2BY1 on C68K_MV, and src/tls and
    # tests/crypto68k are callers that must agree with this directory.
    target_compile_definitions(crypto68k PUBLIC
        C68K_ASM=1 C68K_MV=1
        C68K_ASM_PRIM=1 C68K_ASM_CHACHA20=1 C68K_ASM_POLY1305=1
        C68K_ASM_AES=1 C68K_ASM_25519=1 C68K_ASM_P256=1)

    # Per file, the arch it is written for.  C68K_MV_SUFFIX is what keeps the
    # two addmul_1 twins apart in one archive (c68k_variant.h).
    set_source_files_properties(c68k_prim.S PROPERTIES
        COMPILE_DEFINITIONS "C68K_ASM=1;C68K_MV=1;C68K_MV_SUFFIX=_mulu"
        COMPILE_OPTIONS "-m68020")
    set_source_files_properties(c68k_prim_mulw.S PROPERTIES
        COMPILE_DEFINITIONS "C68K_ASM_MULW=1;C68K_MV=1;C68K_MV_SUFFIX=_mulw"
        COMPILE_OPTIONS "-m68000")
    set_source_files_properties(c68k_poly1305.S PROPERTIES
        COMPILE_DEFINITIONS "C68K_ASM=1;C68K_MV=1"
        COMPILE_OPTIONS "-m68020")
    set_source_files_properties(c68k_aes.S PROPERTIES
        COMPILE_DEFINITIONS "C68K_ASM=1;C68K_MV=1"
        COMPILE_OPTIONS "-m68020")
    set_source_files_properties(c68k_chacha20.S PROPERTIES
        COMPILE_DEFINITIONS "C68K_ASM=1;C68K_MV=1"
        COMPILE_OPTIONS "-m68000")
    set_source_files_properties(c68k_dispatch.S PROPERTIES
        COMPILE_DEFINITIONS "C68K_MV=1"
        COMPILE_OPTIONS "-m68000")
endif()

message(STATUS "  crypto68k asm . ${AMINETXDUO_CRYPTO68K_ASM}")
message(STATUS "  crypto68k mulw asm . ${AMINETXDUO_CRYPTO68K_ASM_MULW}")
message(STATUS "  crypto68k mv .. ${CRYPTO68K_MV}")
