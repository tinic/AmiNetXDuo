# anxnet.device: one SANA-II driver for the whole NE2000/DP8390 card family.
#
# netdev_device.c carries the romtag and the entry guard and must stay FIRST in
# the object order, same rule as the three .library images: the
# "moveq #-1,d0 / rts" has to land at offset 0 of the first code hunk.
#
# Deliberately standalone.  It talks to exec.library and expansion.library and
# nothing else -- no ThreadX, no NetX Duo, no src/config -- because it is a
# SANA-II *provider*, the far side of src/sana2, and a driver that dragged the
# stack in could not be loaded by any other stack.

option(AMINETXDUO_NETDEV_LINK
       "Link DEVS:Networks/anxnet.device, the NE2000/DP8390 family driver" ON)

set(NETDEV_SOURCES
    netdev_device.c     # keep first: romtag, entry guard, Open/BeginIO
    netdev_cmds.c       # the S2_* table
    netdev_bus.c        # register stride and the data port
    netdev_cards.c      # the card table
    netdev_mcaf.c       # the multicast hash, host-testable
    dp8390.c            # NetBSD chip core
    ne2000.c            # NetBSD remote-DMA glue
    ed.c)               # NetBSD mapped-buffer glue: Hydra, ASDG LAN Rover

if(AMINETXDUO_NETDEV_LINK AND CMAKE_CROSSCOMPILING)
    add_executable(anxnet_device ${NETDEV_SOURCES})
    set_target_properties(anxnet_device PROPERTIES
        OUTPUT_NAME "anxnet.device")

    target_include_directories(anxnet_device PRIVATE
        ${AMINETXDUO_CORE_INCLUDES}
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_SOURCE_DIR}/src/sana2")   # sana2_device.h, the command numbers

    # No aminetxduo_common: that library defines its own SysBase handling and
    # pulls the logging macros, and this image defines SysBase itself.
    target_link_options(anxnet_device PRIVATE -nostartfiles -Wl,--gc-sections)

    if(CMAKE_STRIP AND NOT AMINETXDUO_KEEP_SYMBOLS)
        add_custom_command(TARGET anxnet_device POST_BUILD
            COMMAND "${CMAKE_STRIP}" "$<TARGET_FILE:anxnet_device>"
            VERBATIM)
    endif()

    # The same HUNK check the libraries use: magic, resident name list, and
    # hunk 0 is HUNK_CODE.  It takes a path, so the name of the parameter is
    # the only thing about it that says "library".
    add_custom_command(TARGET anxnet_device POST_BUILD
        COMMAND "${CMAKE_COMMAND}"
                "-DLIBRARY_BINARY=$<TARGET_FILE:anxnet_device>"
                -P "${CMAKE_SOURCE_DIR}/cmake/check-library-entry.cmake"
        VERBATIM)

    if(AMIGA_SIZE)
        add_custom_command(TARGET anxnet_device POST_BUILD
            COMMAND "${AMIGA_SIZE}" "$<TARGET_FILE:anxnet_device>"
            COMMENT "anxnet.device size")
    endif()
endif()

# ---------------------------------------------------------------- host test --
#
# netdev_mcaf.c is the multicast hash and nothing else, which is why it is its
# own file: it needs no chip, no bus and no Amiga, and it is the arithmetic
# that decides whether an IPv6 solicited-node frame is ever delivered.

if(NOT CMAKE_CROSSCOMPILING)
    add_executable(test_netdev_mcaf
        test/test_netdev_mcaf.c
        netdev_mcaf.c)

    target_include_directories(test_netdev_mcaf PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_SOURCE_DIR}/src/config/test/shim")

    target_compile_options(test_netdev_mcaf PRIVATE -Wall -Wextra)

    include(CTest)
    add_test(NAME netdev_mcaf COMMAND test_netdev_mcaf)

    # ed.c is #included by the test, not listed here: read_hdr, ring_copy and
    # write_buf are static and the test would otherwise be changing the code
    # it tests.  netdev_bus.c is linked because attach reaches the registers
    # through it, which is the only part of the probe a host can drive.
    add_executable(test_netdev_ed
        test/test_netdev_ed.c
        netdev_bus.c)

    target_include_directories(test_netdev_ed PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_SOURCE_DIR}/src/config/test/shim")

    target_compile_options(test_netdev_ed PRIVATE -Wall -Wextra)

    add_test(NAME netdev_ed COMMAND test_netdev_ed)
endif()

# Raw-serial tracing through the custom chips, for bring-up on an emulator
# where nothing of ours is open yet.  Off, and not a shipped configuration.
option(AMINETXDUO_NETDEV_TRACE "Trace anxnet.device to the serial port" OFF)
if(AMINETXDUO_NETDEV_TRACE AND TARGET anxnet_device)
    target_compile_definitions(anxnet_device PRIVATE NETDEV_TRACE=1)
endif()
