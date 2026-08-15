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
    netdev_pcmcia.c     # the A600/A1200 slot, which has no autoconfig record
    netdev_mcaf.c       # the multicast hash, host-testable
    netdev_macgen.c     # the derived station address, host-testable
    dp8390.c            # NetBSD chip core
    ne2000.c            # NetBSD remote-DMA glue
    ed.c                # NetBSD mapped-buffer glue: Hydra, ASDG LAN Rover
    lance.c             # Am7990/Am79C960: A2065 and Ariadne
    # The data path's bulk moves, from src/net68k rather than a private copy:
    # the movem.l block and its measurements live there.  Two files, the same
    # arrangement as src/net68k's own target -- the .S is #ifdef'd end to end
    # on AMINETXDUO_NET68K_ASM and the .c supplies the symbols when it is off.
    "${CMAKE_SOURCE_DIR}/src/net68k/n68k_iocopy.S"
    "${CMAKE_SOURCE_DIR}/src/net68k/n68k_iocopy.c")

if(AMINETXDUO_NETDEV_LINK AND CMAKE_CROSSCOMPILING)
    add_executable(anxnet_device ${NETDEV_SOURCES})
    set_target_properties(anxnet_device PROPERTIES
        OUTPUT_NAME "anxnet.device")

    target_include_directories(anxnet_device PRIVATE
        ${AMINETXDUO_CORE_INCLUDES}
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_SOURCE_DIR}/src/net68k"   # n68k_iocopy.h, and nothing else
        "${CMAKE_SOURCE_DIR}/src/sana2")   # sana2_device.h, the command numbers

    if(AMINETXDUO_NET68K_ASM)
        set_source_files_properties(
            "${CMAKE_SOURCE_DIR}/src/net68k/n68k_iocopy.S"
            "${CMAKE_SOURCE_DIR}/src/net68k/n68k_iocopy.c"
            PROPERTIES COMPILE_DEFINITIONS AMINETXDUO_NET68K_ASM=1)
        set_source_files_properties(
            "${CMAKE_SOURCE_DIR}/src/net68k/n68k_iocopy.S"
            PROPERTIES COMPILE_OPTIONS "${AMIGA_ARCH_FLAGS}")
    endif()

    # No aminetxduo_common: that library defines its own SysBase handling and
    # pulls the logging macros, and this image defines SysBase itself.
    target_link_options(anxnet_device PRIVATE -nostartfiles -Wl,--gc-sections)

    # -O2 over the tree's -Os, the same trade AMINETXDUO_HOT_O2 makes for the
    # eight profiled NetX Duo files: this is interrupt-path code and the
    # receive service is the whole throughput figure.  Appended, so it wins.
    target_compile_options(anxnet_device PRIVATE -O2)

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

    # The card table needs no chip either, and it is the one file the config
    # parser shares a fact with: the CARD= names.
    add_executable(test_netdev_cards
        test/test_netdev_cards.c
        netdev_cards.c)

    target_include_directories(test_netdev_cards PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_SOURCE_DIR}/include"
        "${CMAKE_SOURCE_DIR}/src/config/test/shim")

    target_compile_options(test_netdev_cards PRIVATE -Wall -Wextra)

    include(CTest)
    add_test(NAME netdev_mcaf COMMAND test_netdev_mcaf)

    # ed.c is #included by the test, not listed here: read_hdr, ring_copy and
    # write_buf are static and the test would otherwise be changing the code
    # it tests.  netdev_bus.c is linked because attach reaches the registers
    # through it, which is the only part of the probe a host can drive.
    add_executable(test_netdev_ed
        test/test_netdev_ed.c
        netdev_bus.c
        "${CMAKE_SOURCE_DIR}/src/net68k/n68k_iocopy.c")

    target_include_directories(test_netdev_ed PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_SOURCE_DIR}/src/net68k"
        "${CMAKE_SOURCE_DIR}/src/config/test/shim")

    target_compile_options(test_netdev_ed PRIVATE -Wall -Wextra)

    add_test(NAME netdev_ed COMMAND test_netdev_ed)
    add_test(NAME netdev_cards COMMAND test_netdev_cards)

    # The derived station address.  Determinism, the locally-administered and
    # unicast bits, and "two different machines get two different addresses"
    # are all properties an emulator cannot show: its card has a valid PROM,
    # so this path is never reached there.
    add_executable(test_netdev_macgen
        test/test_netdev_macgen.c
        netdev_macgen.c
        netdev_mcaf.c)

    target_include_directories(test_netdev_macgen PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_SOURCE_DIR}/src/config/test/shim")

    target_compile_options(test_netdev_macgen PRIVATE -Wall -Wextra)

    add_test(NAME netdev_macgen COMMAND test_netdev_macgen)

    # The bus accessors, and in particular the CNet16 word-read path for odd
    # registers: pure address arithmetic, and the one part of it a host can
    # pin exactly where an emulator models none of it.
    add_executable(test_netdev_bus
        test/test_netdev_bus.c
        netdev_bus.c
        "${CMAKE_SOURCE_DIR}/src/net68k/n68k_iocopy.c")

    target_include_directories(test_netdev_bus PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_SOURCE_DIR}/src/net68k"
        "${CMAKE_SOURCE_DIR}/src/config/test/shim")

    target_compile_options(test_netdev_bus PRIVATE -Wall -Wextra)

    add_test(NAME netdev_bus COMMAND test_netdev_bus)
endif()

# Raw-serial tracing through the custom chips, for bring-up on an emulator
# where nothing of ours is open yet.  Off, and not a shipped configuration.
option(AMINETXDUO_NETDEV_TIME
       "Report where interrupt time goes, in beam positions, to the serial port"
       OFF)
if(AMINETXDUO_NETDEV_TIME AND TARGET anxnet_device)
    target_compile_definitions(anxnet_device PRIVATE NETDEV_TRACE=1 NETDEV_TIME=1)
endif()

option(AMINETXDUO_NETDEV_TRACE "Trace anxnet.device to the serial port" OFF)
if(AMINETXDUO_NETDEV_TRACE AND TARGET anxnet_device)
    target_compile_definitions(anxnet_device PRIVATE NETDEV_TRACE=1)
endif()
