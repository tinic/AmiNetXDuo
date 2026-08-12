# crypto68k on-Amiga tests.
#
#   cmake -S . -B build/cm -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-m68k-amigaos.cmake \
#         -DCMAKE_BUILD_TYPE=Release
#   cmake --build build/cm --parallel --target crypto68k_test crypto68k_bench
#
#   AMINETXDUO_RUN_TAG=rsa20 ./tools/amiberry-run.sh        -t 300 build/cm/tests/crypto68k/crypto68k_test
#   AMINETXDUO_RUN_TAG=rsa30 ./tools/amiberry-run.sh -c 68030 -t 300 build/cm/tests/crypto68k/crypto68k_test
#   AMINETXDUO_RUN_TAG=rsb20 ./tools/amiberry-run.sh        -t 600 build/cm/tests/crypto68k/crypto68k_bench
#   AMINETXDUO_RUN_TAG=rsb30 ./tools/amiberry-run.sh -c 68030 -t 600 build/cm/tests/crypto68k/crypto68k_bench
#
# Two executables, not one, because they have very different run times: the
# correctness pass fits the harness's usual timeout, while the benchmark has to
# run a REFERENCE RSA-2048 private operation without CRT, which alone is over
# two minutes on the floor target.  Splitting them means a correctness
# regression is caught in a short run.
#
# SPDX-License-Identifier: MIT

# The on-Amiga executables below are cross-only: c68k_support.c and
# c68k_timer.c are RawDoFmt and timer.device, and they link aminetxduo_common,
# which does not compile on a host.  Guarded so that a host configure, the
# one CI uses for the parser/VM/vector tests, has a buildable `all`.
# The host tier of the same vectors is at the bottom of this file.
if(CMAKE_CROSSCOMPILING)

add_library(crypto68k_testsupport STATIC
    c68k_support.c
    c68k_timer.c)
target_include_directories(crypto68k_testsupport PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(crypto68k_testsupport PUBLIC crypto68k)

if(AMIGA_NDK_INCLUDE)
    target_include_directories(crypto68k_testsupport PUBLIC "${AMIGA_NDK_INCLUDE}")
endif()

add_executable(crypto68k_test  c68k_test.c)
add_executable(crypto68k_bench c68k_bench.c)

# The P-256 pair.  Split from the RSA pair for the same reason those two are
# split from each other: a correctness regression should be catchable in a
# short run, and the benchmark has to run REFERENCE elliptic curve operations
# that take seconds each on the floor target.
#
#   AMINETXDUO_RUN_TAG=ec20  ./tools/amiberry-run.sh        -t 400 build/cm/tests/crypto68k/crypto68k_ec_test
#   AMINETXDUO_RUN_TAG=ecb20 ./tools/amiberry-run.sh        -t 400 build/cm/tests/crypto68k/crypto68k_ec_bench
#   AMINETXDUO_RUN_TAG=ecb30 ./tools/amiberry-run.sh -c 68030 -t 400 build/cm/tests/crypto68k/crypto68k_ec_bench
# The 25519 field arithmetic, checked ON THE MACHINE.
#
# host/test_c68k_25519.c cannot cover c68k_25519.S at all: it runs portable C
# on the build host, and the assembly is m68k and is DIFFERENT code per part --
# MULU.L on a 68020/68040, four MULU.W on a 68000/68060.  So the same class of
# check has to execute on the guest, which is what this is.
#
# It compares every routine against the portable C the same file still
# compiles, on random inputs INCLUDING the saturated ones, because that is the
# check that found the original fe_fold bug: it dropped a carry out of the top
# limb, lost exactly 38, and passed every published RFC vector, since with
# lazy reduction 0 is routinely carried as 2^256-38.
add_executable(crypto68k_25519_test c68k_25519_test.c)

add_executable(crypto68k_ec_test  c68k_ec_test.c)
add_executable(crypto68k_ec_bench c68k_ec_bench.c)

# The bulk path: AES-128-CBC and SHA-256, every variant timed and checked in
# one run, with the instruction-cost kernels the design decisions rest on.
#
#   AMINETXDUO_RUN_TAG=bulk ./tools/amiberry-run.sh -t 600 -k 56 \
#       build/cm/tests/crypto68k/crypto68k_bulk
#
# c68k_bulk_kernels.S is compiled here rather than in src/crypto68k because it
# is a measurement of the machine and not part of the library.  It needs the
# arch flag explicitly for the same reason the library's .S files do: the
# toolchain file's CMAKE_C_FLAGS_INIT does not reach the assembler, and
# without -m68020 the scaled (An,Dn.W*4) addressing mode does not exist.
#
# NOT BUILT FOR THE 68000, for the same reason tests/perf/cpucal is not: the
# kernels are a measurement of what a 68020 charges for MULU.L and for its
# scaled index, and on a machine that has neither there is nothing to measure.
# The AES and SHA-256 correctness checks in the other four programs cover the
# 68000; only this one is instruction-cost work.  docs/RESEARCH.md §45.
set(CRYPTO68K_PROGS crypto68k_test crypto68k_bench crypto68k_25519_test
    crypto68k_ec_test
                    crypto68k_ec_bench)
if(NOT AMINETXDUO_CPU STREQUAL "68000")
    add_executable(crypto68k_bulk c68k_bulk_bench.c c68k_bulk_kernels.S)
    set_source_files_properties(c68k_bulk_kernels.S PROPERTIES
        COMPILE_OPTIONS "${AMIGA_ARCH_FLAGS}")
    list(APPEND CRYPTO68K_PROGS crypto68k_bulk)
endif()

foreach(prog ${CRYPTO68K_PROGS})
    target_link_libraries(${prog} PRIVATE
        crypto68k_testsupport crypto68k aminetxduo_common)
    set_target_properties(${prog} PROPERTIES OUTPUT_NAME "${prog}")
    if(AMIGA_SIZE)
        add_custom_command(TARGET ${prog} POST_BUILD
            COMMAND "${AMIGA_SIZE}" "$<TARGET_FILE:${prog}>"
            COMMENT "${prog} size")
    endif()
endforeach()

# ------------------------------------------------------ crypto68k vs AmiSSL --
#
# AmiSSL is the AmigaOS OpenSSL port and is what every other TLS client on this
# machine already uses, so it is the baseline that decides whether src/crypto68k
# is worth having.  This program runs both on identical inputs in one process.
#
# It is OFF unless AMINETXDUO_AMISSL_SDK names an unpacked AmiSSL SDK, because
# the SDK is a 2.5 MB third-party download that this repository does not vendor:
#
#   curl -LO https://github.com/jens-maus/amissl/releases/download/5.27/AmiSSL-5.27-SDK.lha
#   curl -LO https://github.com/jens-maus/amissl/releases/download/5.27/AmiSSL-5.27-OS3.lha
#   lha x AmiSSL-5.27-SDK.lha ; lha x AmiSSL-5.27-OS3.lha
#   cmake ... -DAMINETXDUO_AMISSL_SDK=<where>/AmiSSL/Developer
#
# Nothing of AmiSSL's is LINKED.  Every OpenSSL call is an inline macro from
# <inline/amissl.h> that jumps through a library base, so no archive built by
# another compiler has to be ABI-compatible with this one, and every measured
# cycle ran inside amissl_v362.library.  See c68k_amissl_ossl.c.
#
# The run needs the library staged as well as built:
#
#   mkdir -p build/amissl-libs/AmiSSL
#   cp <os3>/AmiSSL/Libs/AmigaOS3/amisslmaster.library            build/amissl-libs/
#   cp <os3>/AmiSSL/Libs/AmigaOS3/AmiSSL/68020-40/amissl_v362.library build/amissl-libs/AmiSSL/
#   AMINETXDUO_RUN_TAG=am ./tools/amiberry-run.sh -t 900 \
#       build/cm-tls/tests/crypto68k/crypto68k_amissl build/amissl-libs
#
# ...with build/amissl-libs renamed to `libs` on the way in, because the
# emulated machine resolves the library through LIBS:.  tools/amissl-run.sh
# does all of that.
set(AMINETXDUO_AMISSL_SDK "" CACHE PATH
    "Unpacked AmiSSL SDK (the directory holding include/ and Autodocs/)")

if(AMINETXDUO_AMISSL_SDK AND TARGET nx_crypto)
    if(NOT EXISTS "${AMINETXDUO_AMISSL_SDK}/include/proto/amissl.h")
        message(FATAL_ERROR
            "AMINETXDUO_AMISSL_SDK=${AMINETXDUO_AMISSL_SDK} has no "
            "include/proto/amissl.h, point it at AmiSSL/Developer.")
    endif()

    add_executable(crypto68k_amissl c68k_amissl_bench.c c68k_amissl_ossl.c)
    target_link_libraries(crypto68k_amissl PRIVATE
        crypto68k_testsupport crypto68k nx_crypto aminetxduo_common)
    target_include_directories(crypto68k_amissl PRIVATE
        "${AMINETXDUO_AMISSL_SDK}/include")
    # AmiSSL's headers switch on this to pick the m68k inline stubs over the
    # AmigaOS 4 interfaces.
    target_compile_definitions(crypto68k_amissl PRIVATE __amigaos3__=1)
    if(AMIGA_SIZE)
        add_custom_command(TARGET crypto68k_amissl POST_BUILD
            COMMAND "${AMIGA_SIZE}" "$<TARGET_FILE:crypto68k_amissl>"
            COMMENT "crypto68k_amissl size")
    endif()
    message(STATUS "  crypto68k vs AmiSSL . ${AMINETXDUO_AMISSL_SDK}")
elseif(AMINETXDUO_AMISSL_SDK)
    message(STATUS "  crypto68k vs AmiSSL . SKIPPED (needs AMINETXDUO_TLS=ON "
                   "for nx_crypto's AES and SHA-256)")
endif()

endif() # CMAKE_CROSSCOMPILING


# ---------------------------------------------------------------- host tier --
#
# The same two correctness programs, built for the host and registered with
# ctest.  c68k_test.c and c68k_ec_test.c are ordinary C; what ties them to the
# Amiga is the support layer (RawDoFmt logging, timer.device, the crash guard),
# and host/ replaces exactly that, see host/c68k_host.c.
#
# This is worth having because it is where the vectors actually get run in CI:
# the Amiga tier needs an emulator and a Kickstart ROM, neither of which exists
# in a public runner, so without this the RSA-2048 known answers and the
# differential comparison against the vendored huge-number code would be tested
# on no push at all.
#
# What the host tier does NOT cover: the hand-written 68020 assembly.  It
# cannot be assembled here, so the host always exercises the portable C path
# (AMINETXDUO_CRYPTO68K_ASM is OFF by default off-target for that reason).  The
# assembly remains an emulator-tier test.
#
# The sources are compiled in directly rather than linked from the crypto68k
# target, because that target's dependency chain reaches aminetxduo_common and
# therefore the NDK.  Same pattern as src/config, src/mbuf and src/bpf.

# NX_CRYPTO_STANDALONE_ENABLE is nx_crypto's own switch for building the
# crypto library with no NetX Duo underneath it, nx_crypto.h then takes its
# types from nx_crypto_port.h, and host/shim/ supplies one whose LONG/ULONG are
# fixed-width 32 bit.  That is not a host liberty, it is the m68k widths: the
# huge-number code needs HN_UBASE2 to be exactly twice HN_UBASE, and every
# stock port spells HN_UBASE as `unsigned long`, which is 64 bits here.  See
# the header comment in host/shim/nx_crypto_port.h.
if(NOT CMAKE_CROSSCOMPILING)

    add_executable(test_crypto68k
        host/test_c68k_host.c
        "${CMAKE_SOURCE_DIR}/src/crypto68k/c68k_prim.c"
        "${CMAKE_SOURCE_DIR}/src/crypto68k/c68k_div.c"
        "${CMAKE_SOURCE_DIR}/src/crypto68k/c68k_mont.c"
        "${CMAKE_SOURCE_DIR}/src/crypto68k/c68k_powm.c"
        "${CMAKE_SOURCE_DIR}/src/crypto68k/c68k_crt.c"
        "${CMAKE_SOURCE_DIR}/src/crypto68k/c68k_aes.c"
        "${CMAKE_SOURCE_DIR}/src/crypto68k/c68k_sha256.c"
        "${CMAKE_SOURCE_DIR}/src/crypto68k/c68k_chacha20.c"
        "${CMAKE_SOURCE_DIR}/src/crypto68k/c68k_poly1305.c"
        "${AMINETXDUO_NETXDUO}/crypto_libraries/src/nx_crypto_huge_number.c"
        # Defines _NX_CRYPTO_INITIALIZE_, which is what gives the library its
        # one definition of _nx_crypto_memset_ptr / _nx_crypto_memcpy_ptr.
        "${AMINETXDUO_NETXDUO}/crypto_libraries/src/nx_crypto_initialize.c")

    target_include_directories(test_crypto68k PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/host/shim"     # nx_crypto_port.h, first
        "${CMAKE_CURRENT_SOURCE_DIR}"               # c68k_vectors.h
        "${CMAKE_SOURCE_DIR}/src/crypto68k"
        "${AMINETXDUO_NETXDUO}/crypto_libraries/inc")

    target_compile_definitions(test_crypto68k PRIVATE
        NX_CRYPTO_STANDALONE_ENABLE)

    # The 25519 vectors are their own executable rather than another section of
    # test_c68k_host.c: c68k_25519.c shares nothing with the huge-number code
    # no nx_crypto types, no NX_CRYPTO_STANDALONE_ENABLE, no port shim --
    # and the only thing it needs from this tree is a SHA-512, which
    # nx_crypto_sha5.c supplies.  Keeping them separate means a failure names
    # which half broke.
    add_executable(test_crypto68k_25519
        host/test_c68k_25519.c
        "${CMAKE_SOURCE_DIR}/src/crypto68k/c68k_25519.c"
        "${AMINETXDUO_NETXDUO}/crypto_libraries/src/nx_crypto_sha5.c"
        # nx_crypto_sha5.c calls through _nx_crypto_memcpy_ptr, and this file
        # is where the library defines it, the same reason test_crypto68k
        # already compiles it in.
        "${AMINETXDUO_NETXDUO}/crypto_libraries/src/nx_crypto_initialize.c")

    target_include_directories(test_crypto68k_25519 PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/host/shim"     # nx_crypto_port.h, first
        "${CMAKE_SOURCE_DIR}/src/crypto68k"
        "${AMINETXDUO_NETXDUO}/crypto_libraries/inc")

    target_compile_definitions(test_crypto68k_25519 PRIVATE
        NX_CRYPTO_STANDALONE_ENABLE)

    include(CTest)
    add_test(NAME crypto68k_vectors COMMAND test_crypto68k)
    add_test(NAME crypto68k_25519   COMMAND test_crypto68k_25519)

endif()
