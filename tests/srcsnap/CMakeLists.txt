# An IPv6 source chosen without nx_ip_protection and used with it (audit
# N-039): the UDP send, the UDP named-source send and the TCP connect are the
# shipped files; ThreadX, the source lookup, the checksum, the IP send and the
# SYN send are stubbed in the test, which deletes or reassigns the source at a
# fixed unlocked point.  Same include order and definitions as tests/syncache.
#
# -fsanitize=bounds traps an index past nx_ipv6_address[] (the named-source
# cases); trapping needs no sanitizer runtime.  The UDP counters are on, as in
# the product, so a refusal can be shown to give them back.
#
# SPDX-License-Identifier: MIT

if(CMAKE_CROSSCOMPILING OR NOT AMINETXDUO_IPV6)
    return()
endif()

set(_nxs "${AMINETXDUO_NETXDUO}/common/src")
add_executable(test_ipv6_source_snapshot
    host/test_ipv6_source_snapshot_host.c
    "${_nxs}/nxd_udp_socket_send.c"
    "${_nxs}/nxd_udp_socket_source_send.c"
    "${_nxs}/nxd_tcp_client_socket_connect.c")

target_include_directories(test_ipv6_source_snapshot PRIVATE
    "${CMAKE_SOURCE_DIR}/tests/perf/host/shim"          # tx_port.h, first
    "${AMINETXDUO_NETXDUO}/ports/linux/gnu/inc"         # nx_port.h
    "${CMAKE_SOURCE_DIR}/port/netxduo-amiga/inc"        # nx_user.h, ours
    "${AMINETXDUO_NETXDUO}/common/inc"
    "${AMINETXDUO_THREADX}/common/inc")

target_compile_definitions(test_ipv6_source_snapshot PRIVATE
    NX_PACKET_ALIGNMENT=8
    NX_INCLUDE_USER_DEFINE_FILE
    NX_IPV6_UTIL_INLINE
    AMINETXDUO_HOST_VENDORED_TX_PORT="${AMINETXDUO_THREADX}/ports/linux/gnu/inc/tx_port.h")

target_compile_options(test_ipv6_source_snapshot PRIVATE -Wall -Wextra
    -fsanitize=bounds -fsanitize-undefined-trap-on-error)
set_target_properties(test_ipv6_source_snapshot PROPERTIES C_STANDARD 11)

include(CTest)
foreach(arm IN ITEMS udp_capture udp_window udp_reused udp_live udp_index
                    tcp_window tcp_reused tcp_live tcp_index)
    add_test(NAME ipv6_source_snapshot_${arm} COMMAND test_ipv6_source_snapshot ${arm})
endforeach()
