# _nx_ip_interface_detach on the host (audit N-014, N-015, N-024).  The detach and
# the ND interface sweep are the shipped files; everything they call that
# needs a stack is stubbed in the test.  Same include order and definitions
# as tests/syncache.
#
# -fsanitize=bounds traps on an out-of-range index into a fixed-size member
# array, which is how routes_full sees the compaction read
# nx_ip_routing_table[NX_IP_ROUTING_TABLE_SIZE].  Trapping needs no sanitizer
# runtime.
#
# SPDX-License-Identifier: MIT

if(CMAKE_CROSSCOMPILING)
    return()
endif()

set(_nxs "${AMINETXDUO_NETXDUO}/common/src")
add_executable(test_ip_detach
    host/test_ip_detach_host.c
    "${_nxs}/nx_ip_interface_detach.c")
if(AMINETXDUO_IPV6)
    target_sources(test_ip_detach PRIVATE
        "${_nxs}/nx_nd_cache_interface_entries_delete.c"
        "${_nxs}/nx_mld_group_join.c"
        "${_nxs}/nx_mld_utility.c")
endif()

target_include_directories(test_ip_detach PRIVATE
    "${CMAKE_SOURCE_DIR}/tests/perf/host/shim"          # tx_port.h, first
    "${AMINETXDUO_NETXDUO}/ports/linux/gnu/inc"         # nx_port.h
    "${CMAKE_SOURCE_DIR}/port/netxduo-amiga/inc"        # nx_user.h, ours
    "${AMINETXDUO_NETXDUO}/common/inc"
    "${AMINETXDUO_THREADX}/common/inc")

target_compile_definitions(test_ip_detach PRIVATE
    NX_PACKET_ALIGNMENT=8
    NX_INCLUDE_USER_DEFINE_FILE
    NX_IPV6_UTIL_INLINE
    AMINETXDUO_HOST_VENDORED_TX_PORT="${AMINETXDUO_THREADX}/ports/linux/gnu/inc/tx_port.h")

target_compile_options(test_ip_detach PRIVATE -Wall -Wextra
    -fsanitize=bounds -fsanitize-undefined-trap-on-error)
set_target_properties(test_ip_detach PROPERTIES C_STANDARD 11)

include(CTest)
add_test(NAME ip_detach_routes      COMMAND test_ip_detach routes)
add_test(NAME ip_detach_routes_full COMMAND test_ip_detach routes_full)
if(AMINETXDUO_IPV6)
    add_test(NAME ip_detach_ndlock  COMMAND test_ip_detach ndlock)
    add_test(NAME ip_detach_mld     COMMAND test_ip_detach mld)
endif()
