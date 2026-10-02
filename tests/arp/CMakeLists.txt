# ARP periodic walk (audit N-007), on the host.  The ARP files are the
# shipped ones; ThreadX, the request send and the packet release are stubbed
# in the test.  Same include order and definitions as tests/syncache.
#
# SPDX-License-Identifier: MIT

if(CMAKE_CROSSCOMPILING)
    return()
endif()

set(_nxs "${AMINETXDUO_NETXDUO}/common/src")
add_executable(test_arp_periodic
    host/test_arp_periodic_host.c
    "${_nxs}/nx_arp_enable.c"
    "${_nxs}/nx_arp_entry_allocate.c"
    "${_nxs}/nx_arp_dynamic_entry_delete.c"
    "${_nxs}/nx_arp_entry_delete.c"
    "${_nxs}/nx_arp_periodic_update.c")

target_include_directories(test_arp_periodic PRIVATE
    "${CMAKE_SOURCE_DIR}/tests/perf/host/shim"          # tx_port.h, first
    "${AMINETXDUO_NETXDUO}/ports/linux/gnu/inc"         # nx_port.h
    "${CMAKE_SOURCE_DIR}/port/netxduo-amiga/inc"        # nx_user.h, ours
    "${AMINETXDUO_NETXDUO}/common/inc"
    "${AMINETXDUO_THREADX}/common/inc")

target_compile_definitions(test_arp_periodic PRIVATE
    NX_PACKET_ALIGNMENT=8
    NX_INCLUDE_USER_DEFINE_FILE
    NX_IPV6_UTIL_INLINE
    AMINETXDUO_HOST_VENDORED_TX_PORT="${AMINETXDUO_THREADX}/ports/linux/gnu/inc/tx_port.h")

target_compile_options(test_arp_periodic PRIVATE -Wall -Wextra)
set_target_properties(test_arp_periodic PROPERTIES C_STANDARD 11)

include(CTest)
add_test(NAME arp_periodic_expiry  COMMAND test_arp_periodic expiry)
add_test(NAME arp_periodic_deleted COMMAND test_arp_periodic deleted)
