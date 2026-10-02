# _nxd_ipv6_address_delete and the TCP connections using the address
# (audit N-038).  The delete, the connection reset and the four suspension
# cleanups it runs are the shipped files; ThreadX, the packet release, the
# multicast leave and the SYN-cache flush are stubbed in the test.  Same
# include order and definitions as tests/syncache.
#
# SPDX-License-Identifier: MIT

if(CMAKE_CROSSCOMPILING OR NOT AMINETXDUO_IPV6)
    return()
endif()

set(_nxs "${AMINETXDUO_NETXDUO}/common/src")
add_executable(test_ipv6_delete_tcp
    host/test_ipv6_delete_tcp_host.c
    "${_nxs}/nxd_ipv6_address_delete.c"
    "${_nxs}/nx_tcp_socket_connection_reset.c"
    "${_nxs}/nx_tcp_socket_block_cleanup.c"
    "${_nxs}/nx_tcp_transmit_cleanup.c"
    "${_nxs}/nx_tcp_receive_cleanup.c"
    "${_nxs}/nx_tcp_connect_cleanup.c"
    "${_nxs}/nx_tcp_disconnect_cleanup.c"
    "${_nxs}/nx_tcp_socket_transmit_queue_flush.c")

target_include_directories(test_ipv6_delete_tcp PRIVATE
    "${CMAKE_SOURCE_DIR}/tests/perf/host/shim"          # tx_port.h, first
    "${AMINETXDUO_NETXDUO}/ports/linux/gnu/inc"         # nx_port.h
    "${CMAKE_SOURCE_DIR}/port/netxduo-amiga/inc"        # nx_user.h, ours
    "${AMINETXDUO_NETXDUO}/common/inc"
    "${AMINETXDUO_THREADX}/common/inc")

target_compile_definitions(test_ipv6_delete_tcp PRIVATE
    NX_PACKET_ALIGNMENT=8
    NX_INCLUDE_USER_DEFINE_FILE
    NX_IPV6_UTIL_INLINE
    AMINETXDUO_HOST_VENDORED_TX_PORT="${AMINETXDUO_THREADX}/ports/linux/gnu/inc/tx_port.h")

target_compile_options(test_ipv6_delete_tcp PRIVATE -Wall -Wextra)
set_target_properties(test_ipv6_delete_tcp PROPERTIES C_STANDARD 11)

include(CTest)
foreach(arm IN ITEMS resets exclusions selfdelete)
    add_test(NAME ipv6_delete_tcp_${arm} COMMAND test_ipv6_delete_tcp ${arm})
endforeach()
