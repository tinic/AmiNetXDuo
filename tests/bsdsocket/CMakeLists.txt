# Host tests for src/bsdsocket, which had none until 2026-08-04 despite being
# 24,827 lines and the whole of the socket API.
#
# What makes them possible is host/shim: bsdsocket_internal.h reaches tx_api.h,
# nx_api.h and thirteen Amiga headers, and ThreadX and NetX Duo both ship Linux
# ports that answer the first two.  host_prelude.h is force-included and its
# comment says why the order in it matters.
#
# NOT every file compiles this way.  options.c, select.c and tcp_handler.c read
# tv_secs and tv_micro out of the Amiga's struct timeval, which collides with
# the POSIX one the C library has already defined; host/shim/devices/timer.h
# records that boundary.
#
# SPDX-License-Identifier: MIT

if(NOT CMAKE_CROSSCOMPILING)
    add_executable(test_inet host/test_inet_host.c
                             "${CMAKE_SOURCE_DIR}/src/bsdsocket/inet.c")

    target_include_directories(test_inet PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/host/shim"
        "${CMAKE_SOURCE_DIR}/src/bsdsocket"
        "${CMAKE_SOURCE_DIR}/include"
        "${AMINETXDUO_THREADX}/common/inc"
        "${AMINETXDUO_THREADX}/ports/linux/gnu/inc"
        "${AMINETXDUO_NETXDUO}/common/inc"
        "${AMINETXDUO_NETXDUO}/ports/linux/gnu/inc"
        "${CMAKE_SOURCE_DIR}/port/netxduo-amiga/inc")

    target_compile_options(test_inet PRIVATE
        -include "${CMAKE_CURRENT_SOURCE_DIR}/host/shim/host_prelude.h")

    add_test(NAME inet_conversions COMMAND test_inet)
endif()
