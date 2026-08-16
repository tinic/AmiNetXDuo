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

# x86_64 only, and the reason is in ThreadX's own port header: tx_port.h for
# linux types LONG as `int` on __x86_64__ and as `long` everywhere else.  On
# arm64 that is eight bytes where the Amiga's is four, so every structure this
# links against would have a different shape than the one that ships, and the
# shim's own exec/types.h collides with it outright.  A test that cannot agree
# with the target about the width of a LONG is not measuring the target.
if(NOT CMAKE_CROSSCOMPILING AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
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

    # routing.c, for ChangeRouteTagList's grammar and its refusals.  The four
    # NetX Duo entry points it uses are stubbed in the test rather than linked;
    # the header of test_route_host.c says what each stub reproduces and from
    # which line of the vendored source.
    add_executable(test_route host/test_route_host.c
                              "${CMAKE_SOURCE_DIR}/src/bsdsocket/routing.c")

    target_include_directories(test_route PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/host/shim"
        "${CMAKE_SOURCE_DIR}/src/bsdsocket"
        "${CMAKE_SOURCE_DIR}/include"
        "${AMINETXDUO_THREADX}/common/inc"
        "${AMINETXDUO_THREADX}/ports/linux/gnu/inc"
        "${AMINETXDUO_NETXDUO}/common/inc"
        "${AMINETXDUO_NETXDUO}/ports/linux/gnu/inc"
        "${CMAKE_SOURCE_DIR}/port/netxduo-amiga/inc")

    target_compile_options(test_route PRIVATE
        -include "${CMAKE_CURRENT_SOURCE_DIR}/host/shim/host_prelude.h")

    # Without this the Linux nx_port.h skips port/netxduo-amiga/inc/nx_user.h,
    # NX_ENABLE_IP_STATIC_ROUTING is not set, NX_IP carries no routing table
    # and routing.c compiles to its "no static routing in this build" branches.
    # The test would then pass against code the Amiga never runs.  The AmigaOS
    # nx_port.h includes nx_user.h unconditionally and needs no such flag.
    target_compile_definitions(test_route PRIVATE NX_INCLUDE_USER_DEFINE_FILE)

    add_test(NAME route_change COMMAND test_route)
endif()
