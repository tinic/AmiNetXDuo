# Milestone 8: the IPv6 dual stack, over the in-tree simulated RAM driver.
#
# Only built with -DAMINETXDUO_IPV6=ON (tests/CMakeLists.txt gates it), because
# the whole test is IPv6.  Run it with
#
#   AMINETXDUO_RUN_TAG=v6 ./tools/fsuae-run.sh -t 300 build/v6/tests/ipv6/ipv6_test
#
# It needs no network at all: the two NX_IP instances on the simulated wire are
# both ends, which makes this the one IPv6 test that cannot be affected by what
# happens to be outside the emulator.  (FS-UAE's SLIRP does turn out to carry
# IPv6, see ipv6_link_test, but that was not known when this was written
# and is not something to depend on.)

add_executable(ipv6_test ipv6_test.c)

target_link_libraries(ipv6_test PRIVATE netxduo_port)

if(AMIGA_NDK_INCLUDE)
    target_include_directories(ipv6_test PRIVATE "${AMIGA_NDK_INCLUDE}")
endif()

set_target_properties(ipv6_test PROPERTIES OUTPUT_NAME "ipv6_test")

if(AMIGA_SIZE)
    add_custom_command(TARGET ipv6_test POST_BUILD
        COMMAND "${AMIGA_SIZE}" "$<TARGET_FILE:ipv6_test>"
        COMMENT "ipv6_test size")
endif()

# The companion that drives the whole stack, config file, SANA-II shim,
# 0x86DD reader, neighbour discovery, against a real device, and reports
# what the wire actually has rather than asserting what it should have.
#
#   AMINETXDUO_RUN_TAG=v6link ./tests/ipv6/run-fsuae.sh -b build/v6

add_executable(ipv6_link_test ipv6_link_test.c)

target_link_libraries(ipv6_link_test PRIVATE
    aminetxduo_netstack netxduo_port)

if(AMIGA_NDK_INCLUDE)
    target_include_directories(ipv6_link_test PRIVATE "${AMIGA_NDK_INCLUDE}")
endif()

set_target_properties(ipv6_link_test PROPERTIES OUTPUT_NAME "ipv6_link_test")

if(AMIGA_SIZE)
    add_custom_command(TARGET ipv6_link_test POST_BUILD
        COMMAND "${AMIGA_SIZE}" "$<TARGET_FILE:ipv6_link_test>"
        COMMENT "ipv6_link_test size")
endif()

# AF_INET6 through the bsdsocket.library ABI, linked against none of our
# code, exactly like tests/libraries/library_test.
#
#   AMINETXDUO_RUN_TAG=v6sock ./tests/ipv6/run-socket-fsuae.sh -b build/v6

# tapdev.c gives this test its own interface, so it needs no a2065.device and
# runs anywhere tier 2 runs.  It links nothing from src/; it includes
# src/sana2/sana2_device.h for the SANA-II command numbers and structures
# alone, which are protocol facts and whose alternative is a second copy of
# them, the same trade tests/tcpdrill states for itself.  What the test's own
# code takes from this tree is still <aminetxduo/cmsg.h> and nothing else.
add_executable(ipv6_socket_test
    ipv6_socket_test.c
    "${CMAKE_SOURCE_DIR}/tests/tcpdrill/tapdev.c")

# include/ for aminetxduo/cmsg.h, RFC 3542 is struct shapes and macros, not
# vectors, so the header is the only thing this program takes from us.  Having
# it on the path here is also the check that it compiles standalone.
target_include_directories(ipv6_socket_test PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/tests/tcpdrill"
    "${CMAKE_SOURCE_DIR}/src/sana2")

if(AMIGA_NDK_INCLUDE)
    target_include_directories(ipv6_socket_test PRIVATE "${AMIGA_NDK_INCLUDE}")
endif()

set_target_properties(ipv6_socket_test PROPERTIES OUTPUT_NAME "ipv6_socket_test")

if(AMINETXDUO_BSDSOCKET_LINK)
    add_dependencies(ipv6_socket_test bsdsocket_library)
endif()

# ---------------------------------------------------------------- host tier --
#
# Router advertisement processing and router solicitation retransmission,
# compiled into a host binary and driven directly.  Same shim as
# tests/netstack/host: the linux port's nx_port.h first, then
# port/netxduo-amiga/inc so nx_user.h is the one that ships.  AMINETXDUO_IPV6
# is a global definition (the top-level CMakeLists), so nx_user.h enables IPv6
# here without anything further.
#
# It answers what a capture cannot: an advertisement with a chosen combination
# of prefix flags, and a day of solicitation retransmission in a few
# microseconds.

if(NOT CMAKE_CROSSCOMPILING)

    add_executable(test_ipv6_ra
        host/test_ipv6_ra_host.c
        "${AMINETXDUO_NETXDUO}/common/src/nx_icmpv6_process_ra.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_icmpv6_validate_ra.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_icmpv6_validate_options.c"
        "${AMINETXDUO_NETXDUO}/common/src/nxd_ipv6_router_solicitation_check.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_ipv6_util.c")

    target_include_directories(test_ipv6_ra PRIVATE
        "${CMAKE_SOURCE_DIR}/tests/perf/host/shim"      # tx_port.h, first
        "${AMINETXDUO_NETXDUO}/ports/linux/gnu/inc"     # nx_port.h
        "${CMAKE_SOURCE_DIR}/port/netxduo-amiga/inc"    # nx_user.h, ours
        "${AMINETXDUO_NETXDUO}/common/inc"
        "${AMINETXDUO_THREADX}/common/inc")

    target_compile_definitions(test_ipv6_ra PRIVATE
        NX_INCLUDE_USER_DEFINE_FILE
        AMINETXDUO_HOST_VENDORED_TX_PORT="${AMINETXDUO_THREADX}/ports/linux/gnu/inc/tx_port.h")

    include(CTest)
    add_test(NAME ipv6_ra COMMAND test_ipv6_ra)

    # The ICMPv6 Packet Too Big receive path and the destination table under
    # it.  Same shim.  It answers what a lab network cannot be made to answer:
    # a path that is genuinely narrower, and a forged error message.

    add_executable(test_ipv6_ptb
        host/test_ipv6_ptb_host.c
        "${AMINETXDUO_NETXDUO}/common/src/nx_icmpv6_process_packet_too_big.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_icmpv6_dest_table_add.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_icmpv6_dest_table_find.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_icmpv6_destination_table_periodic_update.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_ipv6_util.c")

    target_include_directories(test_ipv6_ptb PRIVATE
        "${CMAKE_SOURCE_DIR}/tests/perf/host/shim"      # tx_port.h, first
        "${AMINETXDUO_NETXDUO}/ports/linux/gnu/inc"     # nx_port.h
        "${CMAKE_SOURCE_DIR}/port/netxduo-amiga/inc"    # nx_user.h, ours
        "${AMINETXDUO_NETXDUO}/common/inc"
        "${AMINETXDUO_THREADX}/common/inc")

    target_compile_definitions(test_ipv6_ptb PRIVATE
        NX_INCLUDE_USER_DEFINE_FILE
        AMINETXDUO_HOST_VENDORED_TX_PORT="${AMINETXDUO_THREADX}/ports/linux/gnu/inc/tx_port.h")

    add_test(NAME ipv6_ptb COMMAND test_ipv6_ptb)

    # Inbound fragment reassembly, the queue and the sweep under it, and the
    # pool reserve that bounds what a datagram which never completes can hold.
    # Same shim.  A lab network will not drop the tail of a datagram on cue,
    # nor arrive with the pool already spent.

    add_executable(test_ipv6_frag
        host/test_ipv6_frag_host.c
        "${AMINETXDUO_NETXDUO}/common/src/nx_ipv6_process_fragment_option.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_ip_fragment_assembly.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_ip_fragment_timeout_check.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_ipv6_util.c")

    target_include_directories(test_ipv6_frag PRIVATE
        "${CMAKE_SOURCE_DIR}/tests/perf/host/shim"      # tx_port.h, first
        "${AMINETXDUO_NETXDUO}/ports/linux/gnu/inc"     # nx_port.h
        "${CMAKE_SOURCE_DIR}/port/netxduo-amiga/inc"    # nx_user.h, ours
        "${AMINETXDUO_NETXDUO}/common/inc"
        "${AMINETXDUO_THREADX}/common/inc")

    target_compile_definitions(test_ipv6_frag PRIVATE
        NX_INCLUDE_USER_DEFINE_FILE
        AMINETXDUO_HOST_VENDORED_TX_PORT="${AMINETXDUO_THREADX}/ports/linux/gnu/inc/tx_port.h")

    add_test(NAME ipv6_frag COMMAND test_ipv6_frag)

endif()
