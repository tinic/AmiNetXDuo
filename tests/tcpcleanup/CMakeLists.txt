# The transmit-suspension cleanup's verdict per TCP state (audit N-032).
# nx_tcp_transmit_cleanup.c is the shipped file; ThreadX's resume and the
# packet release are stubbed in the test.  Same include order and
# definitions as tests/syncache.
#
# SPDX-License-Identifier: MIT

if(CMAKE_CROSSCOMPILING)
    return()
endif()

add_executable(test_tcp_transmit_cleanup
    host/test_tcp_transmit_cleanup_host.c
    "${AMINETXDUO_NETXDUO}/common/src/nx_tcp_transmit_cleanup.c")

target_include_directories(test_tcp_transmit_cleanup PRIVATE
    "${CMAKE_SOURCE_DIR}/tests/perf/host/shim"          # tx_port.h, first
    "${AMINETXDUO_NETXDUO}/ports/linux/gnu/inc"         # nx_port.h
    "${CMAKE_SOURCE_DIR}/port/netxduo-amiga/inc"        # nx_user.h, ours
    "${AMINETXDUO_NETXDUO}/common/inc"
    "${AMINETXDUO_THREADX}/common/inc")

target_compile_definitions(test_tcp_transmit_cleanup PRIVATE
    NX_PACKET_ALIGNMENT=8
    NX_INCLUDE_USER_DEFINE_FILE
    AMINETXDUO_HOST_VENDORED_TX_PORT="${AMINETXDUO_THREADX}/ports/linux/gnu/inc/tx_port.h")

target_compile_options(test_tcp_transmit_cleanup PRIVATE -Wall -Wextra)
set_target_properties(test_tcp_transmit_cleanup PROPERTIES C_STANDARD 11)

include(CTest)
add_test(NAME tcp_transmit_cleanup COMMAND test_tcp_transmit_cleanup)
