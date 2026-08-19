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

    # library.c, for the expunge refusal.  The whole translation unit is
    # compiled, not a copy of the function, so what runs is the shipping
    # bsd_lib_expunge(); the test defines every extern it names, including
    # netstack_can_unload(), which is the switch the cases flip.
    #
    # AMINETXDUO_TCPDEVICE because the shipping library is built with it and
    # the expunge has a decline of its own behind that #ifdef.  Compiling it
    # out here would leave the ordering of the four refusals untested.
    add_executable(test_expunge host/test_expunge_host.c
                                "${CMAKE_SOURCE_DIR}/src/bsdsocket/library.c")

    target_include_directories(test_expunge PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/host/shim"
        "${CMAKE_SOURCE_DIR}/src/bsdsocket"
        "${CMAKE_SOURCE_DIR}/src/net68k"        # net68k.h, n68k_cpu_select()
        "${CMAKE_SOURCE_DIR}/include"
        "${CMAKE_BINARY_DIR}/include"           # generated version.h
        "${AMINETXDUO_THREADX}/common/inc"
        "${AMINETXDUO_THREADX}/ports/linux/gnu/inc"
        "${AMINETXDUO_NETXDUO}/common/inc"
        "${AMINETXDUO_NETXDUO}/ports/linux/gnu/inc"
        "${CMAKE_SOURCE_DIR}/port/netxduo-amiga/inc")

    target_compile_options(test_expunge PRIVATE
        -include "${CMAKE_CURRENT_SOURCE_DIR}/host/shim/host_prelude.h")

    # The pointer <-> ULONG round trips in the child-base cloning and in the
    # poisoned vector table.  Both are four bytes on m68k and eight here, and
    # neither is on a path this binary runs; same reason test_sana2_rx carries
    # them.
    target_compile_options(test_expunge PRIVATE
        -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast)

    target_compile_definitions(test_expunge PRIVATE AMINETXDUO_TCPDEVICE)

    add_test(NAME expunge_refusal COMMAND test_expunge)
endif()

# bpf_write() is the only BPF vector that reaches the NetX packet pool. Its
# injector also takes nx_ip_protection so interface shutdown cannot drain a TX
# slot between the claim and BeginIO(). Compile the shipping vector wrapper
# with stubs that make the enter/write/leave ordering observable. Unlike the
# larger bsdsocket host tests above, this needs no live Linux ThreadX objects,
# so the target-width tx_port shim lets it run on arm64 hosts as well.
if(NOT CMAKE_CROSSCOMPILING)
    # transfer.c's connected-UDP readiness and FIONREAD paths inspect packets
    # before nx_udp_socket_receive() removes the UDP header.  This compiles the
    # same small parser the library uses, so those layout assumptions run in
    # ordinary host CTest rather than only in the guest IPv6 socket suite.
    add_executable(test_udp_queue
        host/test_udp_queue_host.c
        "${CMAKE_SOURCE_DIR}/src/bsdsocket/udp_queue.c")

    target_include_directories(test_udp_queue PRIVATE
        "${CMAKE_SOURCE_DIR}/tests/perf/host/shim"
        "${CMAKE_SOURCE_DIR}/src/bsdsocket"
        "${AMINETXDUO_THREADX}/common/inc"
        "${AMINETXDUO_NETXDUO}/common/inc"
        "${AMINETXDUO_NETXDUO}/ports/linux/gnu/inc"
        "${CMAKE_SOURCE_DIR}/port/netxduo-amiga/inc")

    target_compile_definitions(test_udp_queue PRIVATE
        AMINETXDUO_HOST_VENDORED_TX_PORT="${AMINETXDUO_THREADX}/ports/linux/gnu/inc/tx_port.h")

    add_test(NAME udp_queue_layout COMMAND test_udp_queue)

    add_executable(test_bpf_write_serial
        host/test_bpf_write_serial_host.c
        "${CMAKE_SOURCE_DIR}/src/bsdsocket/bpf.c")

    target_include_directories(test_bpf_write_serial PRIVATE
        "${CMAKE_SOURCE_DIR}/tests/perf/host/shim"
        "${CMAKE_CURRENT_SOURCE_DIR}/host/shim"
        "${CMAKE_SOURCE_DIR}/src/bsdsocket"
        "${CMAKE_SOURCE_DIR}/include"
        "${AMINETXDUO_THREADX}/common/inc"
        "${AMINETXDUO_NETXDUO}/common/inc"
        "${AMINETXDUO_NETXDUO}/ports/linux/gnu/inc"
        "${CMAKE_SOURCE_DIR}/port/netxduo-amiga/inc")

    target_compile_options(test_bpf_write_serial PRIVATE
        -include "${CMAKE_CURRENT_SOURCE_DIR}/host/shim/host_prelude.h"
        $<$<OR:$<C_COMPILER_ID:Clang>,$<C_COMPILER_ID:AppleClang>>:-Wno-macro-redefined>)

    target_compile_definitions(test_bpf_write_serial PRIVATE
        AMI_BPF_REPLICA
        AMINETXDUO_HOST_VENDORED_TX_PORT="${AMINETXDUO_THREADX}/ports/linux/gnu/inc/tx_port.h")

    add_test(NAME bpf_write_serialization COMMAND test_bpf_write_serial)
endif()
