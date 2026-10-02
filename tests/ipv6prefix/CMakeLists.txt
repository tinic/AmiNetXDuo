# The IPv6 prefix list and the prefix compare on the host (audit N-021,
# N-022).  The prefix-list add, delete and delete-entry files, the table
# init and nx_ipv6_util.c are the shipped files; the multicast leave the
# delete-entry path can reach is stubbed in the test.  No NX_IPV6_UTIL_INLINE:
# the shipping build uses the function forms in nx_ipv6_util.c.
#
# SPDX-License-Identifier: MIT

if(CMAKE_CROSSCOMPILING OR NOT AMINETXDUO_IPV6)
    return()
endif()

set(_nxs "${AMINETXDUO_NETXDUO}/common/src")
add_executable(test_ipv6_prefix
    host/test_ipv6_prefix_host.c
    "${_nxs}/nx_ipv6_prefix_list_add_entry.c"
    "${_nxs}/nx_ipv6_prefix_list_delete.c"
    "${_nxs}/nx_ipv6_prefix_list_delete_entry.c"
    "${_nxs}/nxd_ipv6_default_router_table_init.c"
    "${_nxs}/nx_ipv6_util.c")

target_include_directories(test_ipv6_prefix PRIVATE
    "${CMAKE_SOURCE_DIR}/tests/perf/host/shim"          # tx_port.h, first
    "${AMINETXDUO_NETXDUO}/ports/linux/gnu/inc"         # nx_port.h
    "${CMAKE_SOURCE_DIR}/port/netxduo-amiga/inc"        # nx_user.h, ours
    "${AMINETXDUO_NETXDUO}/common/inc"
    "${AMINETXDUO_THREADX}/common/inc")

target_compile_definitions(test_ipv6_prefix PRIVATE
    NX_PACKET_ALIGNMENT=8
    NX_INCLUDE_USER_DEFINE_FILE
    AMINETXDUO_HOST_VENDORED_TX_PORT="${AMINETXDUO_THREADX}/ports/linux/gnu/inc/tx_port.h")

target_compile_options(test_ipv6_prefix PRIVATE -Wall -Wextra)
set_target_properties(test_ipv6_prefix PROPERTIES C_STANDARD 11)

include(CTest)
add_test(NAME ipv6_prefix_canonical COMMAND test_ipv6_prefix canonical)
add_test(NAME ipv6_prefix_range     COMMAND test_ipv6_prefix range)
