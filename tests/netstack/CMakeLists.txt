# Milestone 3 bring-up test: the whole stack against a real SANA-II device.
#
# Needs a machine (or an emulator) with an Ethernet card and a
# DEVS:NetInterfaces/<name> file, see tools/amiberry-run.sh -N a2065, which wires an
# FS-UAE A2065 up to SLIRP.

add_executable(netstack_test netstack_test.c)

target_link_libraries(netstack_test PRIVATE
    aminetxduo_netstack netxduo_port)

if(AMIGA_NDK_INCLUDE)
    target_include_directories(netstack_test PRIVATE "${AMIGA_NDK_INCLUDE}")
endif()

set_target_properties(netstack_test PROPERTIES OUTPUT_NAME "netstack_test")

if(AMIGA_SIZE)
    add_custom_command(TARGET netstack_test POST_BUILD
        COMMAND "${AMIGA_SIZE}" "$<TARGET_FILE:netstack_test>"
        COMMENT "netstack_test size")
endif()

# The DHCP lease lifecycle and RFC 3927.  This one reaches into the singleton
# it has to read the live NX_DHCP record and shorten its timers, because
# SLIRP's lease is 24 hours and never expires, so it gets src/netstack/ on
# its include path.  See tests/netstack/run-dhcp3927.sh.
add_executable(dhcp3927_test dhcp3927_test.c)

target_link_libraries(dhcp3927_test PRIVATE
    aminetxduo_netstack netxduo_port)

target_include_directories(dhcp3927_test PRIVATE
    "${CMAKE_SOURCE_DIR}/src/netstack")

if(AMIGA_NDK_INCLUDE)
    target_include_directories(dhcp3927_test PRIVATE "${AMIGA_NDK_INCLUDE}")
endif()

set_target_properties(dhcp3927_test PROPERTIES OUTPUT_NAME "dhcp3927_test")

if(AMIGA_SIZE)
    add_custom_command(TARGET dhcp3927_test POST_BUILD
        COMMAND "${AMIGA_SIZE}" "$<TARGET_FILE:dhcp3927_test>"
        COMMENT "dhcp3927_test size")
endif()

# ---------------------------------------------------------------- host tier --
#
# The retransmission retry limit, checked against a socket rather than against
# a network.  It compiles the vendored TCP output path a second time, with
# tests/perf/host/shim's tx_port.h for the m68k's type widths, and with
# port/netxduo-amiga/inc on the include path so nx_user.h is the one that
# ships, and drives the fast periodic timer by hand.  See the file header
# for what that answers that a packet capture cannot.
#
# The linux port's nx_port.h has to come BEFORE port/netxduo-amiga/inc, or the
# m68k one is found first and wants an AmigaOS ThreadX; nx_user.h is picked up
# from the port directory either way, which is the point.

if(NOT CMAKE_CROSSCOMPILING)

    add_executable(test_tcp_retries
        host/test_tcp_retries_host.c
        "${AMINETXDUO_NETXDUO}/common/src/nx_tcp_socket_create.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_tcp_socket_send_internal.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_tcp_socket_retransmit.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_tcp_fast_periodic_processing.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_tcp_socket_connection_reset.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_tcp_timestamp_option_get.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_tcp_socket_block_cleanup.c")

    # Built without IPv6 whatever the tree default is.  This links six TCP
    # sources to test retransmit timing; the IPv6 send path in those same files
    # reaches nx_ipv6_packet_send, which reaches the ICMPv6 destination table
    # and neighbour solicitation, so honouring the default here would mean
    # listing most of the stack.  -U comes after the directory -D, so nx_user.h
    # sees no AMINETXDUO_IPV6 and defines NX_DISABLE_IPV6.
    target_compile_options(test_tcp_retries PRIVATE -UAMINETXDUO_IPV6)

    target_include_directories(test_tcp_retries PRIVATE
        "${CMAKE_SOURCE_DIR}/tests/perf/host/shim"      # tx_port.h, first
        "${AMINETXDUO_NETXDUO}/ports/linux/gnu/inc"     # nx_port.h
        "${CMAKE_SOURCE_DIR}/port/netxduo-amiga/inc"    # nx_user.h, ours
        "${AMINETXDUO_NETXDUO}/common/inc"
        "${AMINETXDUO_THREADX}/common/inc"
        "${CMAKE_SOURCE_DIR}/src/bsdsocket")            # connfail.h, tested

    target_compile_definitions(test_tcp_retries PRIVATE
        NX_INCLUDE_USER_DEFINE_FILE
        AMINETXDUO_HOST_VENDORED_TX_PORT="${AMINETXDUO_THREADX}/ports/linux/gnu/inc/tx_port.h")

    include(CTest)
    add_test(NAME tcp_retries COMMAND test_tcp_retries)

    # Whether a broadcast this host sends is delivered to this host's own
    # sockets.  Same shim as above, and the same reason for -UAMINETXDUO_IPV6:
    # this is an IPv4 question and the v6 halves of these files reach the
    # neighbour cache.  It links the whole UDP send and receive path so the
    # assertion is "the datagram arrived at a bound socket" rather than "a
    # flag was set"; the link driver and the IP thread are the only stubs.
    add_executable(test_bcast_loopback
        host/test_bcast_loopback_host.c
        "${AMINETXDUO_NETXDUO}/common/src/nx_ip_driver_packet_send.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_ip_packet_send.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_ip_header_add.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_ip_route_find.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_ip_checksum_compute.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_ip_packet_checksum_compute.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_ipv4_packet_receive.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_ipv4_option_process.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_ip_dispatch_process.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_icmpv4_send_error_message.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_igmp_multicast_check.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_packet_pool_initialize.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_udp_enable.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_udp_socket_create.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_udp_socket_bind.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_udp_bind_cleanup.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_udp_free_port_find.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_udp_socket_send.c"
        "${AMINETXDUO_NETXDUO}/common/src/nxd_udp_socket_send.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_udp_packet_receive.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_udp_socket_receive.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_udp_receive_cleanup.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_packet_pool_create.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_packet_pool_cleanup.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_packet_allocate.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_packet_copy.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_packet_data_append.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_packet_length_get.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_packet_release.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_packet_transmit_release.c"
        # The _nxe_ wrappers, because bsdsocket.library calls through them and
        # they are where a bad argument is caught.
        "${AMINETXDUO_NETXDUO}/common/src/nxe_packet_pool_create.c"
        "${AMINETXDUO_NETXDUO}/common/src/nxe_packet_allocate.c"
        "${AMINETXDUO_NETXDUO}/common/src/nxe_packet_data_append.c"
        "${AMINETXDUO_NETXDUO}/common/src/nxe_packet_length_get.c"
        "${AMINETXDUO_NETXDUO}/common/src/nxe_packet_release.c"
        "${AMINETXDUO_NETXDUO}/common/src/nxe_udp_enable.c"
        "${AMINETXDUO_NETXDUO}/common/src/nxe_udp_socket_create.c"
        "${AMINETXDUO_NETXDUO}/common/src/nxe_udp_socket_bind.c"
    # The DNS client checks that an answer came from the server it asked.
    "${AMINETXDUO_NETXDUO}/common/src/nxd_udp_source_extract.c"
        "${AMINETXDUO_NETXDUO}/common/src/nxe_udp_socket_send.c"
        "${AMINETXDUO_NETXDUO}/common/src/nxe_udp_socket_receive.c")

    target_compile_options(test_bcast_loopback PRIVATE -UAMINETXDUO_IPV6)

    target_include_directories(test_bcast_loopback PRIVATE
        "${CMAKE_SOURCE_DIR}/tests/perf/host/shim"      # tx_port.h, first
        "${AMINETXDUO_NETXDUO}/ports/linux/gnu/inc"     # nx_port.h
        "${CMAKE_SOURCE_DIR}/port/netxduo-amiga/inc"    # nx_user.h, ours
        "${AMINETXDUO_NETXDUO}/common/inc"
        "${AMINETXDUO_THREADX}/common/inc")

    target_compile_definitions(test_bcast_loopback PRIVATE
        NX_INCLUDE_USER_DEFINE_FILE
        AMINETXDUO_HOST_VENDORED_TX_PORT="${AMINETXDUO_THREADX}/ports/linux/gnu/inc/tx_port.h")

    add_test(NAME bcast_loopback COMMAND test_bcast_loopback)

    # Whether a TCP connect leaves from the source it was named, on a machine
    # with two interfaces on one subnet, the topology the lab guest cannot
    # boot, built here out of two filled-in nx_ip_interface[] entries.  Links
    # the real connect, the real route lookup and the real SYN build, so the
    # assertion is the source address the SYN carried.  -UAMINETXDUO_IPV6 for
    # the same reason as above: this is an IPv4 question.
    add_executable(test_tcp_source_connect
        host/test_tcp_source_connect_host.c
        "${AMINETXDUO_NETXDUO}/common/src/nxd_tcp_client_socket_connect.c"
        "${AMINETXDUO_NETXDUO}/common/src/nxd_tcp_client_socket_source_connect.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_ip_route_find.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_tcp_packet_send_syn.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_tcp_packet_send_control.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_tcp_socket_create.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_packet_pool_create.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_packet_pool_initialize.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_packet_pool_cleanup.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_packet_allocate.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_packet_release.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_tcp_timestamp_option_get.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_packet_transmit_release.c")

    target_compile_options(test_tcp_source_connect PRIVATE -UAMINETXDUO_IPV6)

    target_include_directories(test_tcp_source_connect PRIVATE
        "${CMAKE_SOURCE_DIR}/tests/perf/host/shim"      # tx_port.h, first
        "${AMINETXDUO_NETXDUO}/ports/linux/gnu/inc"     # nx_port.h
        "${CMAKE_SOURCE_DIR}/port/netxduo-amiga/inc"    # nx_user.h, ours
        "${AMINETXDUO_NETXDUO}/common/inc"
        "${AMINETXDUO_THREADX}/common/inc")

    target_compile_definitions(test_tcp_source_connect PRIVATE
        NX_INCLUDE_USER_DEFINE_FILE
        AMINETXDUO_HOST_VENDORED_TX_PORT="${AMINETXDUO_THREADX}/ports/linux/gnu/inc/tx_port.h")

    add_test(NAME tcp_source_connect COMMAND test_tcp_source_connect)

    # RFC 6298's round-trip time estimator: the arithmetic on its own, and the
    # path that feeds it.  Same shim and the same -UAMINETXDUO_IPV6 as above.
    # nx_tcp_socket_state_ack_check.c is here as well as the output path,
    # because "which acknowledgment is a measurement" is the half of section 3
    # that a formula test cannot reach.
    add_executable(test_tcp_rtt
        host/test_tcp_rtt_host.c
        "${AMINETXDUO_NETXDUO}/common/src/nx_tcp_socket_rtt_sample.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_tcp_socket_create.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_tcp_socket_send_internal.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_tcp_socket_state_ack_check.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_tcp_socket_retransmit.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_tcp_socket_transmit_configure.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_tcp_timestamp_option_get.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_tcp_socket_block_cleanup.c")

    target_compile_options(test_tcp_rtt PRIVATE -UAMINETXDUO_IPV6)

    target_include_directories(test_tcp_rtt PRIVATE
        "${CMAKE_SOURCE_DIR}/tests/perf/host/shim"      # tx_port.h, first
        "${AMINETXDUO_NETXDUO}/ports/linux/gnu/inc"     # nx_port.h
        "${CMAKE_SOURCE_DIR}/port/netxduo-amiga/inc"    # nx_user.h, ours
        "${AMINETXDUO_NETXDUO}/common/inc"
        "${AMINETXDUO_THREADX}/common/inc")

    target_compile_definitions(test_tcp_rtt PRIVATE
        NX_INCLUDE_USER_DEFINE_FILE
        AMINETXDUO_HOST_VENDORED_TX_PORT="${AMINETXDUO_THREADX}/ports/linux/gnu/inc/tx_port.h")

    add_test(NAME tcp_rtt COMMAND test_tcp_rtt)

    # RFC 5827 early retransmit: one duplicate acknowledgment is enough when
    # the flight is two, and three are still required when it is four.  The
    # rig cannot see the difference -- a recovery inside one round trip reads
    # as a fetch that lost nothing at the guest's 20 ms clock -- so the
    # threshold is asserted here instead.  Same sources as test_tcp_rtt above,
    # because it is the same acknowledgment path.
    add_executable(test_tcp_earlyretx
        host/test_tcp_earlyretx_host.c
        "${AMINETXDUO_NETXDUO}/common/src/nx_tcp_socket_rtt_sample.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_tcp_socket_create.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_tcp_socket_send_internal.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_tcp_socket_state_ack_check.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_tcp_socket_retransmit.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_tcp_socket_transmit_configure.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_tcp_timestamp_option_get.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_tcp_socket_block_cleanup.c")

    target_compile_options(test_tcp_earlyretx PRIVATE -UAMINETXDUO_IPV6)

    target_include_directories(test_tcp_earlyretx PRIVATE
        "${CMAKE_SOURCE_DIR}/tests/perf/host/shim"      # tx_port.h, first
        "${AMINETXDUO_NETXDUO}/ports/linux/gnu/inc"     # nx_port.h
        "${CMAKE_SOURCE_DIR}/port/netxduo-amiga/inc"    # nx_user.h, ours
        "${AMINETXDUO_NETXDUO}/common/inc"
        "${AMINETXDUO_THREADX}/common/inc")

    target_compile_definitions(test_tcp_earlyretx PRIVATE
        NX_INCLUDE_USER_DEFINE_FILE
        AMINETXDUO_HOST_VENDORED_TX_PORT="${AMINETXDUO_THREADX}/ports/linux/gnu/inc/tx_port.h")

    add_test(NAME tcp_earlyretx COMMAND test_tcp_earlyretx)

    # A sub-MSS peer cannot pin more than a full-MSS window's worth of packets.
    # The window is a byte budget but the pool is spent a whole packet per
    # segment, so without a per-socket packet-count cap a one-byte-at-a-time
    # peer drains the pool against a window that still says it has room.  Drives
    # the real _nx_tcp_socket_state_data_check enqueue path; fails on a build
    # with NX_ENABLE_LOW_WATERMARK off, passes with it on and the cap sized.
    add_executable(test_tcp_rxflood
        host/test_tcp_rxflood_host.c
        "${AMINETXDUO_NETXDUO}/common/src/nx_tcp_socket_state_data_check.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_tcp_socket_create.c")

    target_compile_options(test_tcp_rxflood PRIVATE -UAMINETXDUO_IPV6)

    target_include_directories(test_tcp_rxflood PRIVATE
        "${CMAKE_SOURCE_DIR}/tests/perf/host/shim"      # tx_port.h, first
        "${AMINETXDUO_NETXDUO}/ports/linux/gnu/inc"     # nx_port.h
        "${CMAKE_SOURCE_DIR}/port/netxduo-amiga/inc"    # nx_user.h, ours
        "${AMINETXDUO_NETXDUO}/common/inc"
        "${AMINETXDUO_THREADX}/common/inc")

    target_compile_definitions(test_tcp_rxflood PRIVATE
        NX_INCLUDE_USER_DEFINE_FILE
        AMINETXDUO_HOST_VENDORED_TX_PORT="${AMINETXDUO_THREADX}/ports/linux/gnu/inc/tx_port.h")

    add_test(NAME tcp_rxflood COMMAND test_tcp_rxflood)

    # RFC 1122 4.2.3.4 sender silly-window avoidance: what leaves when the peer
    # hands its window back in slivers, and -- the half that matters more --
    # that a one-byte write into an open window is still not delayed, because
    # this rule must not become the Nagle the tree does not have.  Every peer
    # the lab has runs receiver-side SWS avoidance of its own and cannot
    # produce the increments, so it is asserted here against the real send
    # path.
    add_executable(test_tcp_sws
        host/test_tcp_sws_host.c
        "${AMINETXDUO_NETXDUO}/common/src/nx_tcp_packet_send_control.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_tcp_socket_rtt_sample.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_tcp_socket_send_internal.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_tcp_socket_state_transmit_check.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_tcp_socket_state_ack_check.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_tcp_socket_retransmit.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_tcp_socket_transmit_configure.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_tcp_timestamp_option_get.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_tcp_socket_block_cleanup.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_tcp_socket_create.c")

    target_compile_options(test_tcp_sws PRIVATE -UAMINETXDUO_IPV6)

    target_include_directories(test_tcp_sws PRIVATE
        "${CMAKE_SOURCE_DIR}/tests/perf/host/shim"      # tx_port.h, first
        "${AMINETXDUO_NETXDUO}/ports/linux/gnu/inc"     # nx_port.h
        "${CMAKE_SOURCE_DIR}/port/netxduo-amiga/inc"    # nx_user.h, ours
        "${AMINETXDUO_NETXDUO}/common/inc"
        "${AMINETXDUO_THREADX}/common/inc")

    target_compile_definitions(test_tcp_sws PRIVATE
        NX_INCLUDE_USER_DEFINE_FILE
        AMINETXDUO_HOST_VENDORED_TX_PORT="${AMINETXDUO_THREADX}/ports/linux/gnu/inc/tx_port.h")

    add_test(NAME tcp_sws COMMAND test_tcp_sws)

    # The two receiver-side acknowledgment holes that left a sender in persist
    # backoff, both fixed in 0.25.3 as submodule commits with no test in this
    # tree: an unanswered window probe (tinic/netxduo bfedf066) and a sub-MSS
    # advertisement nothing reopened until the application had drained half the
    # buffer (0a00c482).  Asserted as protocol signatures against the real
    # receive path and the real fast periodic, because they are properties of
    # the state machine and hold on every card -- a throughput threshold under
    # emulation would guard the emulator's timing instead.
    add_executable(test_tcp_persist
        host/test_tcp_persist_host.c
        "${AMINETXDUO_NETXDUO}/common/src/nx_tcp_socket_state_data_check.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_tcp_fast_periodic_processing.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_tcp_packet_send_ack.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_tcp_packet_send_control.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_tcp_sack_option_build.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_tcp_timestamp_option_get.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_tcp_socket_create.c")

    target_compile_options(test_tcp_persist PRIVATE -UAMINETXDUO_IPV6)

    target_include_directories(test_tcp_persist PRIVATE
        "${CMAKE_SOURCE_DIR}/tests/perf/host/shim"      # tx_port.h, first
        "${AMINETXDUO_NETXDUO}/ports/linux/gnu/inc"     # nx_port.h
        "${CMAKE_SOURCE_DIR}/port/netxduo-amiga/inc"    # nx_user.h, ours
        "${AMINETXDUO_NETXDUO}/common/inc"
        "${AMINETXDUO_THREADX}/common/inc")

    target_compile_definitions(test_tcp_persist PRIVATE
        NX_INCLUDE_USER_DEFINE_FILE
        AMINETXDUO_HOST_VENDORED_TX_PORT="${AMINETXDUO_THREADX}/ports/linux/gnu/inc/tx_port.h")

    add_test(NAME tcp_persist COMMAND test_tcp_persist)

    # The third of the three v0.25.3/v0.25.5 window fixes, and the only one
    # that was measured rather than gated: the pool arithmetic that stopped a
    # machine with no Fast RAM advertising a window its own packet pool could
    # not hold.  Both halves of that coupling compile standalone -- the pool
    # sizing in src/netstack/netstack_pool.c, the window in
    # src/bsdsocket/bsdsocket_window.c -- so this drives them over every
    # machine size, packet stride and ENV:ANXDPOOLDIV value at once, which is
    # what a throughput run on one emulator cannot do.
    add_executable(test_pool_window
        host/test_pool_window_host.c
        "${CMAKE_SOURCE_DIR}/src/netstack/netstack_pool.c"
        "${CMAKE_SOURCE_DIR}/src/bsdsocket/bsdsocket_window.c")

    target_include_directories(test_pool_window PRIVATE
        "${CMAKE_SOURCE_DIR}/src/config/test/shim"     # exec/types.h
        "${CMAKE_SOURCE_DIR}/src/bsdsocket"
        "${CMAKE_SOURCE_DIR}/include")

    target_compile_options(test_pool_window PRIVATE -Wall -Wextra -Werror)

    add_test(NAME pool_window COMMAND test_pool_window)

    # Whether a name lookup stops when it is told to, the retransmission
    # ladder on its own, with the DNS client replaced by scripted answers and
    # the clock by a variable.  A black hole, a Ctrl-C and a mistyped name are
    # the three shapes, and none of them can be produced on demand against a
    # real name server.  Same shim as above; no NetX Duo source is linked,
    # because the ladder deliberately knows nothing about DNS.
    add_executable(test_dns_retry
        host/test_dns_retry_host.c
        "${CMAKE_SOURCE_DIR}/src/netstack/netstack_retry.c")

    target_compile_options(test_dns_retry PRIVATE -UAMINETXDUO_IPV6)

    target_include_directories(test_dns_retry PRIVATE
        "${CMAKE_SOURCE_DIR}/tests/perf/host/shim"      # tx_port.h, first
        "${CMAKE_SOURCE_DIR}/src/config/test/shim"      # exec/types.h
        "${CMAKE_SOURCE_DIR}/src/netstack"
        "${CMAKE_SOURCE_DIR}/include"
        "${AMINETXDUO_NETXDUO}/ports/linux/gnu/inc"     # nx_port.h
        "${CMAKE_SOURCE_DIR}/port/netxduo-amiga/inc"    # nx_user.h, ours
        "${AMINETXDUO_NETXDUO}/common/inc"
        "${AMINETXDUO_THREADX}/common/inc")

    target_compile_definitions(test_dns_retry PRIVATE
        NX_INCLUDE_USER_DEFINE_FILE
        AMINETXDUO_HOST_VENDORED_TX_PORT="${AMINETXDUO_THREADX}/ports/linux/gnu/inc/tx_port.h")

    add_test(NAME dns_retry COMMAND test_dns_retry)

    # What a failed lookup is reported as.  TX_NOT_AVAILABLE is the case worth
    # having: it is the DNS client's mutex, not a DNS status, and it used to
    # fall through to "no such host".
    add_executable(test_dns_status
        host/test_dns_status_host.c
        "${CMAKE_SOURCE_DIR}/src/netstack/netstack_dns_status.c")

    target_compile_options(test_dns_status PRIVATE -UAMINETXDUO_IPV6)

    target_include_directories(test_dns_status PRIVATE
        "${CMAKE_SOURCE_DIR}/tests/perf/host/shim"      # tx_port.h, first
        "${CMAKE_SOURCE_DIR}/src/config/test/shim"      # exec/types.h
        "${CMAKE_SOURCE_DIR}/src/netstack"
        "${CMAKE_SOURCE_DIR}/include"
        "${AMINETXDUO_NETXDUO}/ports/linux/gnu/inc"     # nx_port.h
        "${CMAKE_SOURCE_DIR}/port/netxduo-amiga/inc"    # nx_user.h, ours
        "${AMINETXDUO_NETXDUO}/common/inc"
        "${AMINETXDUO_NETXDUO}/addons/dns"
        "${AMINETXDUO_THREADX}/common/inc")

    target_compile_definitions(test_dns_status PRIVATE
        NX_INCLUDE_USER_DEFINE_FILE
        AMINETXDUO_HOST_VENDORED_TX_PORT="${AMINETXDUO_THREADX}/ports/linux/gnu/inc/tx_port.h")

    add_test(NAME dns_status COMMAND test_dns_status)

    # A DHCP BOUND notification runs on the client task while resolver
    # changes belong on a caller task. Permit() in this harness injects a
    # second notification exactly after the first snapshot, proving that it
    # remains pending for the next caller.
    add_executable(test_dns_handoff
        host/test_dns_handoff_host.c
        "${CMAKE_SOURCE_DIR}/src/netstack/netstack_dns_handoff.c")

    target_include_directories(test_dns_handoff PRIVATE
        "${CMAKE_SOURCE_DIR}/tests/perf/host/shim"      # tx_port.h, first
        "${CMAKE_SOURCE_DIR}/tests/bsdsocket/host/shim" # Exec declarations
        "${CMAKE_SOURCE_DIR}/src/netstack"
        "${AMINETXDUO_THREADX}/common/inc")

    target_compile_definitions(test_dns_handoff PRIVATE
        AMINETXDUO_HOST_VENDORED_TX_PORT="${AMINETXDUO_THREADX}/ports/linux/gnu/inc/tx_port.h")

    target_compile_options(test_dns_handoff PRIVATE -Wall -Wextra -Werror)

    add_test(NAME dns_handoff COMMAND test_dns_handoff)

    # A server can be named by both DHCP interfaces, and renewal or lease loss
    # on one must not erase the other's ownership.  Exercise the exact compact
    # per-interface set used by the resolver reconciliation path.
    add_executable(test_dns_lease
        host/test_dns_lease_host.c
        "${CMAKE_SOURCE_DIR}/src/netstack/netstack_dns_lease.c")

    target_include_directories(test_dns_lease PRIVATE
        "${CMAKE_SOURCE_DIR}/src/config/test/shim"     # exec/types.h
        "${CMAKE_SOURCE_DIR}/src/netstack"
        "${CMAKE_SOURCE_DIR}/include")

    target_compile_options(test_dns_lease PRIVATE -Wall -Wextra -Werror)

    add_test(NAME dns_lease COMMAND test_dns_lease)

    # Removing one of two interfaces takes the machine's default gateway with
    # it whenever that interface carried it, and the survivor is then left
    # unable to reach anything off its own subnet.
    add_executable(test_gateway
        host/test_gateway_host.c
        "${CMAKE_SOURCE_DIR}/src/netstack/netstack_gateway.c")

    target_include_directories(test_gateway PRIVATE
        "${CMAKE_SOURCE_DIR}/src/config/test/shim"     # exec/types.h
        "${CMAKE_SOURCE_DIR}/src/netstack"
        "${CMAKE_SOURCE_DIR}/include")

    target_compile_options(test_gateway PRIVATE -Wall -Wextra -Werror)

    add_test(NAME gateway COMMAND test_gateway)

    # A DNSSL suffix can supply the default domain only while that advertised
    # suffix remains live, and never owns a file, DHCP, or caller override.
    add_executable(test_dns_domain
        host/test_dns_domain_host.c
        "${CMAKE_SOURCE_DIR}/src/netstack/netstack_dns_domain.c")

    target_include_directories(test_dns_domain PRIVATE
        "${CMAKE_SOURCE_DIR}/src/config/test/shim"     # exec/types.h
        "${CMAKE_SOURCE_DIR}/src/netstack"
        "${CMAKE_SOURCE_DIR}/include")

    target_compile_options(test_dns_domain PRIVATE -Wall -Wextra -Werror)

    add_test(NAME dns_domain COMMAND test_dns_domain)

    # DHCP option 12 owns the machine name only while a lease that supplied it
    # remains live. Renewals and multiple interfaces share one deterministic
    # owner, and loss restores the lower-ranked name it displaced.
    add_executable(test_dhcp_hostname
        host/test_dhcp_hostname_host.c
        "${CMAKE_SOURCE_DIR}/src/netstack/netstack_dhcp_hostname.c"
        "${CMAKE_SOURCE_DIR}/src/config/config_hostname.c"
        "${CMAKE_SOURCE_DIR}/src/config/config_text.c")

    target_include_directories(test_dhcp_hostname PRIVATE
        "${CMAKE_SOURCE_DIR}/src/config/test/shim"     # exec/types.h
        "${CMAKE_SOURCE_DIR}/src/config"
        "${CMAKE_SOURCE_DIR}/src/netstack"
        "${CMAKE_SOURCE_DIR}/include")

    target_compile_options(test_dhcp_hostname PRIVATE -Wall -Wextra -Werror)

    add_test(NAME dhcp_hostname COMMAND test_dhcp_hostname)

    # The RFC 8106 callbacks run on the IP thread, while a caller task absorbs
    # their state before entering the DNS client.  Permit() in this harness
    # injects a second advertisement exactly after the snapshot, proving that
    # it remains pending and cannot overwrite the snapshot being consumed.
    add_executable(test_ra_handoff
        host/test_ra_handoff_host.c
        "${CMAKE_SOURCE_DIR}/src/netstack/netstack_ra.c")

    target_include_directories(test_ra_handoff PRIVATE
        "${CMAKE_SOURCE_DIR}/tests/perf/host/shim"      # tx_port.h, first
        "${CMAKE_SOURCE_DIR}/tests/bsdsocket/host/shim" # Exec declarations
        "${CMAKE_SOURCE_DIR}/src/netstack"
        "${CMAKE_SOURCE_DIR}/include"
        "${AMINETXDUO_NETXDUO}/ports/linux/gnu/inc"     # nx_port.h
        "${CMAKE_SOURCE_DIR}/port/netxduo-amiga/inc"    # nx_user.h, ours
        "${AMINETXDUO_NETXDUO}/common/inc"
        "${AMINETXDUO_THREADX}/common/inc")

    target_compile_definitions(test_ra_handoff PRIVATE
        AMINETXDUO_IPV6
        NX_INCLUDE_USER_DEFINE_FILE
        AMINETXDUO_HOST_VENDORED_TX_PORT="${AMINETXDUO_THREADX}/ports/linux/gnu/inc/tx_port.h")

    target_compile_options(test_ra_handoff PRIVATE -Wall -Wextra -Werror)

    add_test(NAME ra_handoff COMMAND test_ra_handoff)

    # ---------------------------------------------------------------------
    # netstack.c itself, whole, on the host.
    #
    # host/netstack_host_env.c answers every Exec call, every tx_amiga_* call,
    # every NetX Duo entry point and every other src/ symbol the file names,
    # so what runs is the shipping netstack_startup(), netstack_shutdown(),
    # netstack_can_unload() and netstack_interface_dhcp_start() rather than a
    # copy of any of them.  The environment's header says what is steered and
    # what is merely answered.
    #
    # host/shim/tx_amiga.h and not port/threadx-amiga/inc: that one carries the
    # Amiga tx_port.h, and this tier is the linux ThreadX port with
    # tests/perf/host/shim's m68k type widths on top.  It must come before
    # tests/bsdsocket/host/shim-netstatus is anywhere near the path, which is
    # why no target here puts that directory on it.
    #
    # AMINETXDUO_MDNS, AMINETXDUO_AREXX and AMINETXDUO_BPF are compiled OUT
    # whatever the tree default is, for the reason -UAMINETXDUO_IPV6 is used
    # above: each pulls in another src/netstack translation unit's worth of
    # entry points that these two claims say nothing about.  IPv6 stays ON,
    # because that is the shipping default and its handful of entry points are
    # already answered.
    set(NETSTACK_HOST_TU
        "${CMAKE_SOURCE_DIR}/src/netstack/netstack.c"
        "${CMAKE_SOURCE_DIR}/src/netstack/netstack_pool.c"
        host/netstack_host_env.c)

    set(NETSTACK_HOST_INCLUDES
        "${CMAKE_CURRENT_SOURCE_DIR}/host/shim"         # tx_amiga.h, ours
        "${CMAKE_SOURCE_DIR}/tests/perf/host/shim"      # tx_port.h, first
        "${CMAKE_SOURCE_DIR}/tests/bsdsocket/host/shim" # exec, dos, proto
        "${CMAKE_CURRENT_SOURCE_DIR}/host"
        "${CMAKE_SOURCE_DIR}/src/netstack"
        "${CMAKE_SOURCE_DIR}/src/bsdsocket"
        "${CMAKE_SOURCE_DIR}/src/net68k"                # net68k.h, AMINETXDUO_RX_VERIFY
        "${CMAKE_SOURCE_DIR}/include"
        "${AMINETXDUO_NETXDUO}/ports/linux/gnu/inc"     # nx_port.h
        "${CMAKE_SOURCE_DIR}/port/netxduo-amiga/inc"    # nx_user.h, ours
        "${AMINETXDUO_NETXDUO}/common/inc"
        "${AMINETXDUO_NETXDUO}/addons/dhcp"
        "${AMINETXDUO_NETXDUO}/addons/dns"
        "${AMINETXDUO_NETXDUO}/addons/auto_ip"
        "${AMINETXDUO_NETXDUO}/addons/mdns"
        "${AMINETXDUO_THREADX}/common/inc")

    set(NETSTACK_HOST_DEFINES
        AMINETXDUO_IPV6
        NX_INCLUDE_USER_DEFINE_FILE
        AMINETXDUO_HOST_VENDORED_TX_PORT="${AMINETXDUO_THREADX}/ports/linux/gnu/inc/tx_port.h")

    # -Wno-unknown-warning-option for Clang: netstack.c carries a
    # #pragma GCC diagnostic for -Wanalyzer-use-of-uninitialized-value, which
    # Clang does not have, and under -Werror an unknown group is an error.
    set(NETSTACK_HOST_OPTIONS
        -UAMINETXDUO_MDNS -UAMINETXDUO_AREXX -UAMINETXDUO_BPF
        -Wall -Wextra -Werror
        $<$<OR:$<C_COMPILER_ID:Clang>,$<C_COMPILER_ID:AppleClang>>:-Wno-unknown-warning-option>)

    # The expunge refusal at the joint: that a FAILED tx_amiga_kernel_stop()
    # leaves ami_ns_kernel_started set, which is what makes
    # netstack_can_unload() answer FALSE.  Both halves were already tested
    # separately and neither test moves if the flag is cleared too early; the
    # file header says what that costs.
    add_executable(test_expunge_joint
        host/test_expunge_joint_host.c ${NETSTACK_HOST_TU})

    target_include_directories(test_expunge_joint PRIVATE ${NETSTACK_HOST_INCLUDES})
    target_compile_definitions(test_expunge_joint PRIVATE ${NETSTACK_HOST_DEFINES})
    target_compile_options(test_expunge_joint PRIVATE ${NETSTACK_HOST_OPTIONS})

    add_test(NAME expunge_joint COMMAND test_expunge_joint)

    # netstack_interface_dhcp_start()'s re-arm.  NX_DHCP_ALREADY_STARTED never
    # arises in the arm the lab can run, so stop-and-start-again was never
    # exercised; here the DHCP client answers it on demand.
    add_executable(test_dhcp_restart
        host/test_dhcp_restart_host.c ${NETSTACK_HOST_TU})

    target_include_directories(test_dhcp_restart PRIVATE ${NETSTACK_HOST_INCLUDES})
    target_compile_definitions(test_dhcp_restart PRIVATE ${NETSTACK_HOST_DEFINES})
    target_compile_options(test_dhcp_restart PRIVATE ${NETSTACK_HOST_OPTIONS})

    add_test(NAME dhcp_restart COMMAND test_dhcp_restart)

endif()
