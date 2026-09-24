# The MSS a send is sized by, read two ways: nx_tcp_socket_mss_get() under the
# IP mutex, and _nx_tcp_socket_mss_compute() without it, which bsdsocket.library
# uses when the caller holds the baton.  The shipped
# third_party/netxduo/common/src/nx_tcp_socket_mss_get.c is compiled into the
# test and the two ThreadX mutex calls are stubbed to count, so the test can
# say that the answers agree AND which of the two took the mutex.
#
# Not gated on x86_64, for tests/syncache's reason: no Amiga structure.
#
# SPDX-License-Identifier: MIT

if(CMAKE_CROSSCOMPILING)
    return()
endif()

# Twice: as the library ships, AMINETXDUO_TCP_TIMESTAMP off, and with
# NX_ENABLE_TCP_TIMESTAMP, the one branch the two readings could disagree on.
# nx_user.h only defines it, so defining it here is the whole switch.
# NX_INCLUDE_USER_DEFINE_FILE, or the Linux nx_port.h skips our nx_user.h.
foreach(_t test_tcp_mss test_tcp_mss_ts)
    add_executable(${_t}
        host/test_tcp_mss_host.c
        "${AMINETXDUO_NETXDUO}/common/src/nx_tcp_socket_mss_get.c")

    target_include_directories(${_t} PRIVATE
        "${CMAKE_SOURCE_DIR}/tests/perf/host/shim"          # tx_port.h, first
        "${AMINETXDUO_NETXDUO}/ports/linux/gnu/inc"         # nx_port.h
        "${CMAKE_SOURCE_DIR}/port/netxduo-amiga/inc"        # nx_user.h, ours
        "${AMINETXDUO_NETXDUO}/common/inc"
        "${AMINETXDUO_THREADX}/common/inc")

    target_compile_definitions(${_t} PRIVATE
        NX_INCLUDE_USER_DEFINE_FILE
        AMINETXDUO_HOST_VENDORED_TX_PORT="${AMINETXDUO_THREADX}/ports/linux/gnu/inc/tx_port.h")

    target_compile_options(${_t} PRIVATE -Wall -Wextra)
    set_target_properties(${_t} PROPERTIES C_STANDARD 11)
endforeach()

target_compile_definitions(test_tcp_mss_ts PRIVATE NX_ENABLE_TCP_TIMESTAMP)

add_test(NAME tcp_mss_peek            COMMAND test_tcp_mss)
add_test(NAME tcp_mss_peek_timestamps COMMAND test_tcp_mss_ts)
