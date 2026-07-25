# nx_secure TLS -- built only when AMINETXDUO_TLS is ON.
#
# This is deliberately NOT linked into bsdsocket.library.  docs/RESEARCH.md 9
# records the decision: TLS ships as a separate library behind its own build
# option, and is benchmarked on 68020 before any API is promised.  The gate is
# tests/tls/tls_bench -- see docs/RESEARCH.md 5.5 for the numbers it produced.
#
# Two static libraries, because they have very different dependency shapes:
#
#   nx_crypto  -- crypto_libraries/src.  Compiles against nx_api.h for its
#                 types only; it makes no ThreadX or NetX Duo call at runtime
#                 (verified: zero tx_*/nx_* references in crypto_libraries/src).
#                 That is what makes a primitive benchmark possible without
#                 bringing the scheduler up.
#   nx_secure  -- nx_secure/src.  The TLS/DTLS/X.509 state machines.  These do
#                 need NetX Duo (packets, TCP sockets) and therefore ThreadX.
#
# SPDX-License-Identifier: MIT

set(NX_SECURE_ROOT "${AMINETXDUO_NETXDUO}/nx_secure")
set(NX_CRYPTO_ROOT "${AMINETXDUO_NETXDUO}/crypto_libraries")

option(AMINETXDUO_DTLS "Build the DTLS half of nx_secure (55 files)" OFF)

set(AMINETXDUO_TLS_INCLUDES
    ${AMINETXDUO_CORE_INCLUDES}
    "${NX_SECURE_ROOT}/inc"
    "${NX_SECURE_ROOT}/ports"
    "${NX_CRYPTO_ROOT}/inc")

# ------------------------------------------------- compiler runtime shim ---

# This toolchain ships a ZERO BYTE libgcc.a, so `unsigned long long` division
# does not link.  nx_crypto's huge-number long division needs it.  The single
# copy lives in src/common (aminetxduo_m68k_rt); see the header comment in
# src/common/ami_udivdi3.c for why we supply the helper rather than dropping
# the huge-number radix to 16 bits.
#
# src/common is configured unconditionally and before this directory, so on a
# cross build the target is simply there.  On a host build it is absent and
# libgcc supplies the real helpers, so link nothing.
if(TARGET aminetxduo_m68k_rt)
    set(AMINETXDUO_TLS_RT aminetxduo_m68k_rt)
else()
    set(AMINETXDUO_TLS_RT "")
endif()

# ------------------------------------------------------------- nx_crypto ---

file(GLOB NX_CRYPTO_SOURCES "${NX_CRYPTO_ROOT}/src/*.c")

# The self-test objects are empty translation units unless NX_CRYPTO_SELF_TEST
# is defined, but they still carry ~40 KB of test vectors in .rodata when it is.
# We never define it; keep them out so nobody is surprised by the size.
list(FILTER NX_CRYPTO_SOURCES EXCLUDE REGEX "self_test")

add_library(nx_crypto STATIC ${NX_CRYPTO_SOURCES})
target_include_directories(nx_crypto PUBLIC ${AMINETXDUO_TLS_INCLUDES})
target_link_libraries(nx_crypto PUBLIC netxduo ${AMINETXDUO_TLS_RT})

# ------------------------------------------------------------- nx_secure ---

file(GLOB NX_SECURE_SOURCES "${NX_SECURE_ROOT}/src/*.c")
list(FILTER NX_SECURE_SOURCES EXCLUDE REGEX "self_test")
if(NOT AMINETXDUO_DTLS)
    list(FILTER NX_SECURE_SOURCES EXCLUDE REGEX "/nx_secure_dtls")
endif()

add_library(nx_secure STATIC ${NX_SECURE_SOURCES})
target_include_directories(nx_secure PUBLIC ${AMINETXDUO_TLS_INCLUDES})
target_link_libraries(nx_secure PUBLIC nx_crypto netxduo)

# ------------------------------------------------------------ amiga glue ---

add_library(aminetxduo_tls STATIC tls_amiga.c)
target_include_directories(aminetxduo_tls PUBLIC
    ${AMINETXDUO_TLS_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(aminetxduo_tls PUBLIC nx_secure ${AMINETXDUO_TLS_RT})

if(AMIGA_NDK_INCLUDE)
    target_include_directories(aminetxduo_tls PUBLIC "${AMIGA_NDK_INCLUDE}")
endif()
