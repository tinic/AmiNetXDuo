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
