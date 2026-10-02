# The raw send's contract with the IPv6 address table and the IPv4 route
# lookup (audit N-036, N-037).  The three raw-send files are the shipped
# ones; ThreadX's mutex, the route and source lookups and both IP sends are
# stubbed in the test.  Same include order and definitions as
# tests/syncache.
#
# SPDX-License-Identifier: MIT

if(CMAKE_CROSSCOMPILING OR NOT AMINETXDUO_IPV6)
    return()
endif()

set(_nxs "${AMINETXDUO_NETXDUO}/common/src")
add_executable(test_raw_source_send
    host/test_raw_source_send_host.c
    "${_nxs}/nxd_ip_raw_packet_source_send.c"
    "${_nxs}/nxd_ip_raw_packet_send.c"
    "${_nxs}/nxd_ipv6_raw_packet_send_internal.c")

target_include_directories(test_raw_source_send PRIVATE
    "${CMAKE_SOURCE_DIR}/tests/perf/host/shim"          # tx_port.h, first
    "${AMINETXDUO_NETXDUO}/ports/linux/gnu/inc"         # nx_port.h
    "${CMAKE_SOURCE_DIR}/port/netxduo-amiga/inc"        # nx_user.h, ours
    "${AMINETXDUO_NETXDUO}/common/inc"
    "${AMINETXDUO_THREADX}/common/inc")

target_compile_definitions(test_raw_source_send PRIVATE
    NX_PACKET_ALIGNMENT=8
    NX_INCLUDE_USER_DEFINE_FILE
    AMINETXDUO_HOST_VENDORED_TX_PORT="${AMINETXDUO_THREADX}/ports/linux/gnu/inc/tx_port.h")

target_compile_options(test_raw_source_send PRIVATE -Wall -Wextra)
set_target_properties(test_raw_source_send PROPERTIES C_STANDARD 11)

include(CTest)
foreach(arm IN ITEMS v6_deleted v6_live v6_find v4_noroute v4_route)
    add_test(NAME raw_source_send_${arm} COMMAND test_raw_source_send ${arm})
endforeach()
