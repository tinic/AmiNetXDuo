# Host tests for src/bsdsocket, which had none until 2026-08-04 despite being
# 24,827 lines and the whole of the socket API.
#
# What makes them possible is host/shim: bsdsocket_internal.h reaches tx_api.h,
# nx_api.h and thirteen Amiga headers, and ThreadX and NetX Duo both ship Linux
# ports that answer the first two.  host_prelude.h is force-included and its
# comment says why the order in it matters.
#
# EVERY src/bsdsocket file now compiles this way.  The last two in were
# tcp_handler.c and transfer.c, and they are at the bottom of this file rather
# than in the block below because they need the target's POINTER width and not
# only its LONG: the first passes a struct FileHandle and a session pointer
# through LONG packet arguments, the second pins struct iovec and struct
# msghdr to the 4.4BSD shape.  Both hold at 32 bits and neither can at 64.
# The struct timeval collision that used to exclude options.c and select.c is
# gone as well; host/shim/devices/timer.h says how.
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
    # events.c and not a stub for ami_event(): the expunge is where the event
    # ring's mark is taken back, and a stub would make that line untested in
    # the one harness that runs the real bsd_lib_expunge().
    add_executable(test_expunge host/test_expunge_host.c
                                "${CMAKE_SOURCE_DIR}/src/bsdsocket/library.c"
                                "${CMAKE_SOURCE_DIR}/src/common/events.c")

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

    target_compile_definitions(test_expunge PRIVATE AMINETXDUO_TCPDEVICE
        AMINETXDUO_EVENT_RING=${AMINETXDUO_EVENT_RING})

    add_test(NAME expunge_refusal COMMAND test_expunge)

    # select.c, the readiness predicates and WaitSelect()'s return.
    #
    # WHY THIS ONE IS ON THE PUSH GATE.  Two regressions have shipped from
    # select.c, both green on the push that carried them: a connected UDP
    # socket reported readable by another peer's datagram (d3ccab5d), and
    # WaitSelect() counting ready descriptors where AmiTCP counts ready bits
    # (e6c76f5e, undone by 8aa5a3a7).  tests/ipv6/ipv6_socket_test.c covers
    # both, on the emulator, which .github/workflows/emulator.yml runs on a
    # nightly cron and on a tag.  This tier is what a push runs, so the same
    # claims are asserted here against the shipping translation unit.
    add_executable(test_select host/test_select_host.c
                               "${CMAKE_SOURCE_DIR}/src/bsdsocket/select.c")

    target_include_directories(test_select PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/host/shim"
        "${CMAKE_SOURCE_DIR}/src/bsdsocket"
        "${CMAKE_SOURCE_DIR}/include"
        "${AMINETXDUO_THREADX}/common/inc"
        "${AMINETXDUO_THREADX}/ports/linux/gnu/inc"
        "${AMINETXDUO_NETXDUO}/common/inc"
        "${AMINETXDUO_NETXDUO}/ports/linux/gnu/inc"
        "${CMAKE_SOURCE_DIR}/port/netxduo-amiga/inc")

    target_compile_options(test_select PRIVATE
        -include "${CMAKE_CURRENT_SOURCE_DIR}/host/shim/host_prelude.h")

    # Same reason as test_route: without it the Linux nx_port.h skips
    # port/netxduo-amiga/inc/nx_user.h, and select.c compiles against a
    # different NX_TCP_SOCKET and a different retransmission ladder than the
    # one that ships.
    target_compile_definitions(test_select PRIVATE NX_INCLUDE_USER_DEFINE_FILE)

    add_test(NAME select_readiness COMMAND test_select)

    # rxdirect.c, the completer that runs inside the TCP receive notify.  It
    # is compiled unconditionally here even though the shipping default is
    # -DAMINETXDUO_RX_DIRECT_COMPLETE=OFF: an option nothing builds is an
    # option nothing notices breaking, and the claim under test is a
    # use-after-release against vendored NetX Duo, not a rate.
    add_executable(test_rxdirect host/test_rxdirect_host.c
                                 "${CMAKE_SOURCE_DIR}/src/bsdsocket/rxdirect.c")

    target_include_directories(test_rxdirect PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/host/shim"
        "${CMAKE_SOURCE_DIR}/src/bsdsocket"
        "${CMAKE_SOURCE_DIR}/include"
        "${AMINETXDUO_THREADX}/common/inc"
        "${AMINETXDUO_THREADX}/ports/linux/gnu/inc"
        "${AMINETXDUO_NETXDUO}/common/inc"
        "${AMINETXDUO_NETXDUO}/ports/linux/gnu/inc"
        "${CMAKE_SOURCE_DIR}/port/netxduo-amiga/inc")

    target_compile_options(test_rxdirect PRIVATE
        -include "${CMAKE_CURRENT_SOURCE_DIR}/host/shim/host_prelude.h")

    target_compile_definitions(test_rxdirect PRIVATE NX_INCLUDE_USER_DEFINE_FILE
        AMINETXDUO_RX_DIRECT_COMPLETE)

    add_test(NAME rxdirect_completer COMMAND test_rxdirect)

    # options.c, the other file the struct timeval collision used to keep out.
    # SO_RCVTIMEO and SO_SNDTIMEO are the conversions that were unreachable
    # here, and every option in the file was provable only on the emulator.
    #
    # The four files options.c delegates to -- src/ipv6's option handlers,
    # mcast.c and cmsg.c -- are stubbed; the test's header says what that does
    # and does not prove.
    add_executable(test_sockopt host/test_sockopt_host.c
                                "${CMAKE_SOURCE_DIR}/src/bsdsocket/options.c")

    target_include_directories(test_sockopt PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/host/shim"
        "${CMAKE_SOURCE_DIR}/src/bsdsocket"
        "${CMAKE_SOURCE_DIR}/include"
        "${AMINETXDUO_THREADX}/common/inc"
        "${AMINETXDUO_THREADX}/ports/linux/gnu/inc"
        "${AMINETXDUO_NETXDUO}/common/inc"
        "${AMINETXDUO_NETXDUO}/ports/linux/gnu/inc"
        "${CMAKE_SOURCE_DIR}/port/netxduo-amiga/inc")

    target_compile_options(test_sockopt PRIVATE
        -include "${CMAKE_CURRENT_SOURCE_DIR}/host/shim/host_prelude.h")

    target_compile_definitions(test_sockopt PRIVATE NX_INCLUDE_USER_DEFINE_FILE)

    add_test(NAME socket_options COMMAND test_sockopt)

    # netstatus.c, for NETSTATUS_NEIGHBOURS and the one flag `arp` prints from
    # it. The router flag came from a NetX Duo back pointer that is only
    # written when a router advertisement carried a Source Link-Layer Address
    # option, so whether the segment's own router was flagged came out
    # differently from one boot to the next and no emulator run could hold it
    # still. Here the neighbour cache and the default router table are staged
    # by hand, which is the only way to ask the question at all.
    #
    # host/shim-netstatus/tx_amiga.h exists for this target: netstatus.c
    # includes the real one, which cannot be on the include path here. A
    # directory of its own because tests/tick reaches host/shim as well and
    # wants the REAL tx_amiga.h; the shim's header says so.
    add_executable(test_neighbour host/test_neighbour_host.c
                                  "${CMAKE_SOURCE_DIR}/src/bsdsocket/netstatus.c")

    target_include_directories(test_neighbour PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/host/shim-netstatus"
        "${CMAKE_CURRENT_SOURCE_DIR}/host/shim"
        "${CMAKE_SOURCE_DIR}/src/bsdsocket"
        "${CMAKE_SOURCE_DIR}/include"
        "${AMINETXDUO_THREADX}/common/inc"
        "${AMINETXDUO_THREADX}/ports/linux/gnu/inc"
        "${AMINETXDUO_NETXDUO}/common/inc"
        "${AMINETXDUO_NETXDUO}/ports/linux/gnu/inc"
        "${CMAKE_SOURCE_DIR}/port/netxduo-amiga/inc")

    target_compile_options(test_neighbour PRIVATE
        -include "${CMAKE_CURRENT_SOURCE_DIR}/host/shim/host_prelude.h")

    # AMINETXDUO_IPV6 because the whole neighbour cache is behind it, and
    # NX_INCLUDE_USER_DEFINE_FILE for the reason test_route gives: without it
    # NX_IP has a different shape than the one that ships.
    target_compile_definitions(test_neighbour PRIVATE
        NX_INCLUDE_USER_DEFINE_FILE AMINETXDUO_IPV6)

    add_test(NAME neighbour_router COMMAND test_neighbour)

    # netstatus.c again, for NETSTATUS_DHCP6 and for which netstack entry
    # point NETCTRL_DHCP_RELEASE reaches. Neither was provable on the
    # emulator: the v6-only arm needs a DHCPv6 server on the segment, and the
    # refusal under test is the one that fires when there is no lease, which
    # is indistinguishable from the bug it hid.
    #
    # NETSTATUS_HEALTH is here too, for the tick counters it copies: the
    # stubs that stage them are already in this target.
    add_executable(test_dhcp6 host/test_dhcp6_host.c
                              "${CMAKE_SOURCE_DIR}/src/bsdsocket/netstatus.c")

    target_include_directories(test_dhcp6 PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/host/shim-netstatus"
        "${CMAKE_CURRENT_SOURCE_DIR}/host/shim"
        "${CMAKE_SOURCE_DIR}/src/bsdsocket"
        "${CMAKE_SOURCE_DIR}/include"
        "${AMINETXDUO_THREADX}/common/inc"
        "${AMINETXDUO_THREADX}/ports/linux/gnu/inc"
        "${AMINETXDUO_NETXDUO}/common/inc"
        "${AMINETXDUO_NETXDUO}/ports/linux/gnu/inc"
        "${CMAKE_SOURCE_DIR}/port/netxduo-amiga/inc")

    target_compile_options(test_dhcp6 PRIVATE
        -include "${CMAKE_CURRENT_SOURCE_DIR}/host/shim/host_prelude.h")

    target_compile_definitions(test_dhcp6 PRIVATE
        NX_INCLUDE_USER_DEFINE_FILE AMINETXDUO_IPV6)

    add_test(NAME dhcp6_status_health COMMAND test_dhcp6)

    # library_runtime.c, for the usergroup.library hold.
    #
    # WHY THIS ONE EXISTS.  bsd_usergroup_open() opens usergroup.library and
    # holds it until expunge, and no caller in this tree ever asks it for
    # anything: the clients that need the hold are ixemul's ixnet and the rest
    # of the AmiTCP 4.x world, which reach that library through the base
    # bsdsocket.library is already holding.  A tidy-up that removed an open
    # with no matching use would close the stack for them with nothing here
    # going red, so the hold is asserted against the shipping translation unit.
    add_executable(test_usergroup_hold host/test_usergroup_host.c
                                  "${CMAKE_SOURCE_DIR}/src/bsdsocket/library_runtime.c")

    target_include_directories(test_usergroup_hold PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/host/shim"
        "${CMAKE_SOURCE_DIR}/src/bsdsocket"
        "${CMAKE_SOURCE_DIR}/include"
        "${AMINETXDUO_THREADX}/common/inc"
        "${AMINETXDUO_THREADX}/ports/linux/gnu/inc"
        "${AMINETXDUO_NETXDUO}/common/inc"
        "${AMINETXDUO_NETXDUO}/ports/linux/gnu/inc"
        "${CMAKE_SOURCE_DIR}/port/netxduo-amiga/inc")

    target_compile_options(test_usergroup_hold PRIVATE
        -include "${CMAKE_CURRENT_SOURCE_DIR}/host/shim/host_prelude.h")

    target_compile_definitions(test_usergroup_hold PRIVATE NX_INCLUDE_USER_DEFINE_FILE)

    add_test(NAME usergroup_hold COMMAND test_usergroup_hold)
endif()

# bpf_write() is the only BPF vector that reaches the NetX packet pool. Its
# injector also takes nx_ip_protection so interface shutdown cannot drain a TX
# slot between the claim and BeginIO(). Compile the shipping vector wrapper
# with stubs that make the enter/write/leave ordering observable. Unlike the
# larger bsdsocket host tests above, this needs no live Linux ThreadX objects,
# so the target-width tx_port shim lets it run on arm64 hosts as well.
if(NOT CMAKE_CROSSCOMPILING)
    # transfer.c's connected-UDP readiness and FIONREAD paths inspect packets
    # before nx_udp_socket_receive() removes the UDP header.  netstats.c also
    # reads UDP queue payloads and TCP's distinct sent-list accounting.  This
    # compiles the same small helpers the library uses, so those layout
    # assumptions run in ordinary host CTest rather than only in guest tests.
    add_executable(test_udp_queue
        host/test_udp_queue_host.c
        "${CMAKE_SOURCE_DIR}/src/bsdsocket/udp_queue.c")

    target_include_directories(test_udp_queue PRIVATE
        "${CMAKE_SOURCE_DIR}/tests/perf/host/shim"
        "${CMAKE_SOURCE_DIR}/src/bsdsocket"
        "${CMAKE_SOURCE_DIR}/include"
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

# ---------------------------------------------------------------------------
# The two files that need the TARGET'S POINTER WIDTH, not merely the target's
# LONG.  Both were listed above as outside the tier, and both are outside for
# the same reason rather than for the two different ones the comments used to
# give.
#
#   tcp_handler.c   reaches a `struct FileHandle` through BADDR(dp_Arg1) and
#                   hands its session back through fh_Arg1.  dp_Arg1 and
#                   fh_Arg1 are LONG, so a pointer only survives that round
#                   trip where a pointer is four bytes.  Silencing the casts,
#                   as test_expunge does for two round trips nothing runs,
#                   would leave the packet paths untestable: here they ARE the
#                   code under test.
#
#   transfer.c      opens with eleven _Static_asserts pinning `struct iovec`
#                   to 8 bytes and `struct msghdr` to the 4.4BSD 28-byte shape.
#                   Those were read as "this file can never compile on a
#                   host".  They can: on i386 glibc's own definitions are that
#                   shape to the byte -- {void *, size_t} is 0/4/8, and
#                   msghdr's seven members land on 0,4,8,12,16,20,24 -- so the
#                   assertions hold as written, against the shipping ABI, with
#                   nothing shimmed and nothing conditional.  They fail at 64
#                   bits because the ABI genuinely is not that shape there.
#
# So this block is entered only where sizeof(void *) == 4, which on a CI
# machine means the -m32 configure the `host32` stage makes.  Both targets are
# in HOST32_TEST_TARGETS in tools/ci.sh; without gcc-multilib the stage skips
# and says so.
#
# tests/perf/host/shim comes FIRST on the include path so tx_port.h is the one
# with the m68k's type widths.  The vendored linux tx_port.h types LONG as
# `long` off __x86_64__, and at -m32 that is four bytes but a different type
# from the shim's exec/types.h `int`, which is a redefinition error rather than
# a layout difference.
if(NOT CMAKE_CROSSCOMPILING AND CMAKE_SIZEOF_VOID_P EQUAL 4)
    set(BSD_HOST32_INCLUDES
        "${CMAKE_SOURCE_DIR}/tests/perf/host/shim"      # tx_port.h, first
        "${CMAKE_CURRENT_SOURCE_DIR}/host/shim"
        "${CMAKE_SOURCE_DIR}/src/bsdsocket"
        "${CMAKE_SOURCE_DIR}/include"
        "${AMINETXDUO_THREADX}/common/inc"
        "${AMINETXDUO_NETXDUO}/common/inc"
        "${AMINETXDUO_NETXDUO}/ports/linux/gnu/inc"
        "${CMAKE_SOURCE_DIR}/port/netxduo-amiga/inc")

    set(BSD_HOST32_DEFINES
        NX_INCLUDE_USER_DEFINE_FILE
        AMINETXDUO_HOST_VENDORED_TX_PORT="${AMINETXDUO_THREADX}/ports/linux/gnu/inc/tx_port.h")

    # tcp_handler.c, the TCP: AmigaDOS handler.  The test #includes the
    # translation unit rather than linking it, because the packet vocabulary
    # is two switch statements inside static functions and the path grammar is
    # a static as well; what runs is still the shipping code.  Both loops are
    # driven for real, off a scripted per-port message queue.
    add_executable(test_tcp_handler host/test_tcp_handler_host.c)

    target_include_directories(test_tcp_handler PRIVATE ${BSD_HOST32_INCLUDES})

    target_compile_options(test_tcp_handler PRIVATE
        -include "${CMAKE_CURRENT_SOURCE_DIR}/host/shim/host_prelude.h")

    target_compile_definitions(test_tcp_handler PRIVATE ${BSD_HOST32_DEFINES}
        AMINETXDUO_TCPDEVICE)

    add_test(NAME tcp_handler_packets COMMAND test_tcp_handler)

    # transfer.c, send/sendto/sendmsg and recv/recvfrom/recvmsg.  The whole
    # translation unit, so the _Static_asserts on the scatter/gather ABI run
    # as part of the build rather than being asserted about.
    add_executable(test_transfer host/test_transfer_host.c
                                 "${CMAKE_SOURCE_DIR}/src/bsdsocket/transfer.c")

    target_include_directories(test_transfer PRIVATE ${BSD_HOST32_INCLUDES})

    target_compile_options(test_transfer PRIVATE
        -include "${CMAKE_CURRENT_SOURCE_DIR}/host/shim/host_prelude.h")

    target_compile_definitions(test_transfer PRIVATE ${BSD_HOST32_DEFINES})

    add_test(NAME transfer_scatter_gather COMMAND test_transfer)
endif()
