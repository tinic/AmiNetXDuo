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

# Issue #50: a SYN-cache entry outlives the interface its SYN arrived on.
# The cache, _nx_ip_interface_detach, the TCP control senders, the IPv4 route
# lookup and _nx_ipv6_packet_send are all the shipped files; host_nx_assert.h
# is force-included so NX_ASSERT_FAIL unwinds to the test instead of sleeping
# forever as nx_api.h has it on the target.
#
# The arms assert the correct behaviour: detach and a lone IPv6 address
# delete drop the entries that point at what they zero, send nothing for
# them, leave the cache's lists and counters consistent, and keep entries on
# every other interface (keep).
set(_nxs "${AMINETXDUO_NETXDUO}/common/src")
add_executable(test_syncache_detach
    host/test_syncache_detach_host.c
    "${_nxs}/nx_tcp_syncache.c"
    "${_nxs}/nx_ip_interface_detach.c"
    "${_nxs}/nx_ip_route_find.c"
    "${_nxs}/nx_tcp_packet_send_syn.c"
    "${_nxs}/nx_tcp_packet_send_rst.c"
    "${_nxs}/nx_tcp_packet_send_control.c"
    "${_nxs}/nx_tcp_timestamp_option_get.c")
if(AMINETXDUO_IPV6)
    target_sources(test_syncache_detach PRIVATE
        "${_nxs}/nx_ipv6_packet_send.c"
        "${_nxs}/nxd_ipv6_address_delete.c")
endif()

target_include_directories(test_syncache_detach PRIVATE
    "${CMAKE_SOURCE_DIR}/tests/perf/host/shim"          # tx_port.h, first
    "${AMINETXDUO_NETXDUO}/ports/linux/gnu/inc"         # nx_port.h
    "${CMAKE_SOURCE_DIR}/port/netxduo-amiga/inc"        # nx_user.h, ours
    "${AMINETXDUO_NETXDUO}/common/inc"
    "${AMINETXDUO_THREADX}/common/inc")

target_compile_definitions(test_syncache_detach PRIVATE
    NX_PACKET_ALIGNMENT=8
    NX_INCLUDE_USER_DEFINE_FILE
    NX_IPV6_UTIL_INLINE
    AMINETXDUO_HOST_VENDORED_TX_PORT="${AMINETXDUO_THREADX}/ports/linux/gnu/inc/tx_port.h")

target_compile_options(test_syncache_detach PRIVATE -Wall -Wextra
    -include "${CMAKE_CURRENT_SOURCE_DIR}/host/host_nx_assert.h")
set_target_properties(test_syncache_detach PROPERTIES C_STANDARD 11)

add_test(NAME syncache_detach_v4      COMMAND test_syncache_detach v4)
add_test(NAME syncache_detach_v4reuse COMMAND test_syncache_detach v4reuse)
add_test(NAME syncache_detach_keep    COMMAND test_syncache_detach keep)
if(AMINETXDUO_IPV6)
    add_test(NAME syncache_detach_v6       COMMAND test_syncache_detach v6)
    add_test(NAME syncache_detach_v6accept COMMAND test_syncache_detach v6accept)
    add_test(NAME syncache_detach_v6delete COMMAND test_syncache_detach v6delete)
endif()
