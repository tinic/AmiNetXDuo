# The SYN defence, on the host: the cookie's arithmetic and the cache's
# bookkeeping.  third_party/netxduo/common/src/nx_tcp_syncache.c is compiled
# into the test rather than linked out of the netxduo library, so the test
# binary carries the shipped file and the four functions it calls that need a
# stack are stubbed in the test itself.
#
# tests/perf/host/shim goes first for tx_port.h, which makes ULONG the
# target's 32 bits: every step of the cookie is 32-bit arithmetic, and a host
# with a 64-bit ULONG would test a construction the Amiga never computes.
#
# Not gated on x86_64 the way tests/bsdsocket is: nothing here links an Amiga
# structure, so the LONG-width problem that gate exists for cannot arise.
#
# SPDX-License-Identifier: MIT

if(CMAKE_CROSSCOMPILING)
    return()
endif()

add_executable(test_syncache
    host/test_syncache_host.c
    "${AMINETXDUO_NETXDUO}/common/src/nx_tcp_syncache.c")

target_include_directories(test_syncache PRIVATE
    "${CMAKE_SOURCE_DIR}/tests/perf/host/shim"          # tx_port.h, first
    "${AMINETXDUO_NETXDUO}/ports/linux/gnu/inc"         # nx_port.h
    "${CMAKE_SOURCE_DIR}/port/netxduo-amiga/inc"        # nx_user.h, ours
    "${AMINETXDUO_NETXDUO}/common/inc"
    "${AMINETXDUO_THREADX}/common/inc")

# Without NX_INCLUDE_USER_DEFINE_FILE the Linux nx_port.h skips our nx_user.h
# and the window scale, SACK and timestamp options are all compiled out -- the
# three things the cookie has to carry.  The test would pass against a build
# that negotiates none of them.
# NX_IPV6_UTIL_INLINE makes COPY_IPV6_ADDRESS and CHECK_IPV6_ADDRESSES_SAME
# the inline macros rather than functions.  The shipping build links their
# function forms out of the IPv6 util source, which this test does not compile;
# the macro bodies are the same field-by-field copy and compare, so inlining
# them here links the test without pulling the IPv6 core in and changes nothing
# about what runs on the Amiga.  A dual-stack CI build defines AMINETXDUO_IPV6
# globally, so FEATURE_NX_IPV6 is on here and the IPv6 branch is compiled.
target_compile_definitions(test_syncache PRIVATE
    NX_PACKET_ALIGNMENT=8
    NX_INCLUDE_USER_DEFINE_FILE
    NX_IPV6_UTIL_INLINE
    AMINETXDUO_HOST_VENDORED_TX_PORT="${AMINETXDUO_THREADX}/ports/linux/gnu/inc/tx_port.h")

target_compile_options(test_syncache PRIVATE -Wall -Wextra)
set_target_properties(test_syncache PROPERTIES C_STANDARD 11)

include(CTest)
add_test(NAME syncache_cookie COMMAND test_syncache cookie)
add_test(NAME syncache_cache  COMMAND test_syncache cache)
