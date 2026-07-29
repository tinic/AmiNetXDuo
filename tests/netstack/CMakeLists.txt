# Milestone 3 bring-up test: the whole stack against a real SANA-II device.
#
# Needs a machine (or an emulator) with an Ethernet card and a
# DEVS:NetInterfaces/<name> file -- see tools/fsuae-run.sh -n, which wires an
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
# -- it has to read the live NX_DHCP record and shorten its timers, because
# SLIRP's lease is 24 hours and never expires -- so it gets src/netstack/ on
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
# a network.  It compiles the vendored TCP output path a second time -- with
# tests/perf/host/shim's tx_port.h for the m68k's type widths, and with
# port/netxduo-amiga/inc on the include path so nx_user.h is the one that
# ships -- and drives the fast periodic timer by hand.  See the file header
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
        "${CMAKE_SOURCE_DIR}/port/netxduo-amiga/inc"    # nx_user.h -- ours
        "${AMINETXDUO_NETXDUO}/common/inc"
        "${AMINETXDUO_THREADX}/common/inc")

    target_compile_definitions(test_tcp_retries PRIVATE
        NX_INCLUDE_USER_DEFINE_FILE
        AMINETXDUO_HOST_VENDORED_TX_PORT="${AMINETXDUO_THREADX}/ports/linux/gnu/inc/tx_port.h")

    include(CTest)
    add_test(NAME tcp_retries COMMAND test_tcp_retries)

endif()
