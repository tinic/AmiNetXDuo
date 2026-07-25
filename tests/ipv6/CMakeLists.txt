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
# IPv6 -- see ipv6_link_test -- but that was not known when this was written
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

# The companion that drives the whole stack -- config file, SANA-II shim,
# 0x86DD reader, neighbour discovery -- against a real device, and reports
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

# AF_INET6 through the bsdsocket.library ABI -- linked against none of our
# code, exactly like tests/libraries/library_test.
#
#   AMINETXDUO_RUN_TAG=v6sock ./tests/ipv6/run-socket-fsuae.sh -b build/v6

add_executable(ipv6_socket_test ipv6_socket_test.c)

if(AMIGA_NDK_INCLUDE)
    target_include_directories(ipv6_socket_test PRIVATE "${AMIGA_NDK_INCLUDE}")
endif()

set_target_properties(ipv6_socket_test PROPERTIES OUTPUT_NAME "ipv6_socket_test")

if(AMINETXDUO_BSDSOCKET_LINK)
    add_dependencies(ipv6_socket_test bsdsocket_library)
endif()
