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
    netdev_direct.c     # claim/completion half of the single-copy RX path
    netdev_queue.c      # queued IORequest invariants, host-testable
    netdev_cmds.c       # the S2_* table
    netdev_event.c      # S2_ONEVENT delivery and S2_PacketFilter, host-testable
    netdev_watchdog.c   # progress clock for a full transmit ring
    netdev_mcast.c      # exact multicast membership and range transactions
    netdev_bus.c        # register stride and the data port
    netdev_clock.c      # a wait measured against the beam, not counted in reads
    netdev_cards.c      # the card table
    netdev_pcmcia.c     # the A600/A1200 slot, which has no autoconfig record
    netdev_cis.c        # the PC Card configuration table, host-testable
    netdev_isapnp.c     # the X-Surf's chip, which is behind an ISA PnP bridge
    netdev_diag.c       # the probe record, published for CheckNetDevice
    netdev_mcaf.c       # the multicast hash, host-testable
    netdev_macgen.c     # the derived station address, host-testable
    dp8390.c            # NetBSD chip core
    ne2000.c            # NetBSD remote-DMA glue
    ed.c                # NetBSD mapped-buffer glue: Hydra, ASDG LAN Rover
    lance.c             # Am7990/Am79C960: A2065 and Ariadne
    el3.c               # 3Com EtherLink III: the 3C589 in the PCMCIA slot
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

    # Nothing references a romtag; -flto removes it and the link still
    # succeeds.  See the -flto note in the top-level CMakeLists.txt.  The
    # guard above already means cross-only, so this needs no second one.
    target_link_options(anxnet_device PRIVATE "-Wl,-u,_netdev_romtag")

    # Profile can recover the device's hunk addresses from the segment tag in
    # its base.  Keep the other half of that contract beside the binary: a
    # diagnostic non-LTO build's map and objects name every file-static
    # routine once profreport.py has relocated those hunks.
    target_link_options(anxnet_device PRIVATE
        "-Wl,-Map=$<TARGET_FILE_DIR:anxnet_device>/anxnet.device.map")

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

    # -Os, and this used to be -O2 for the whole target -- "interrupt-path code
    # and the receive service is the whole throughput figure".
    #
    # -O2 costs 15-21 KB here, which is far more than it looked. Built four
    # ways at one commit:
    #
    #                    -O2       -Os
    #     LTO          51,628    30,544
    #     non-LTO      48,636    33,236
    #
    # It is also the whole reason anxnet.device was the one image that GREW
    # under -flto while the other three shrank: at -O2 the LTO inliner is
    # aggressive rather than size-constrained, so it added 3,652 bytes of code
    # (relocations shrank ~670, netting +2,992). At -Os it behaves like
    # everywhere else and comes out 8.1% smaller than non-LTO. LTO was never
    # the anomaly; -O2 on a whole target was.
    #
    # 21 KB is a lot on a 2 MB A1200, but this is a driver and read throughput
    # is the field metric, so size alone did not decide it. Measured on the
    # A1200 + a2065 reference, 18 runs interleaved ABABAB/BABABA/ABABAB, nine
    # per arm, loadavg recorded before each:
    #
    #     read    -O2 592.8 KB/s   -Os 586.2 KB/s   -1.12%, 95% CI -2.5..+0.2
    #     write   -O2 346.7 KB/s   -Os 330.2 KB/s   -4.73%, t=16.8
    #
    # READ DOES NOT SEPARATE. The distributions overlap almost completely --
    # the best -Os run beats seven of the nine -O2 runs, the worst -O2 run is
    # below five of the nine -Os runs -- and the interval on the difference
    # includes zero. The load asymmetry ran AGAINST -O2 (mean 2.54 against
    # 1.98), so it cannot be what produced -O2's edge.
    #
    # WRITE REGRESSES 4.7% AND THAT ONE IS REAL: the two TX ranges do not
    # overlap at all across nine runs each, and the 64 KB size-bounded arm
    # shows the same 5.5%. Recorded rather than hidden. Read is the field
    # metric and a write regression alone does not block a change, so this
    # landed on 21,080 bytes for no measurable read cost.
    target_compile_options(anxnet_device PRIVATE -Os)

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

    # The $VER: string, checked the way every command in src/tools is.  Found
    # by AmigaDOS scanning the file, so no reference exists for the linker to
    # see, and this target links --gc-sections.
    add_custom_command(TARGET anxnet_device POST_BUILD
        COMMAND "${CMAKE_COMMAND}"
                "-DTOOL_BINARY=$<TARGET_FILE:anxnet_device>"
                -P "${CMAKE_SOURCE_DIR}/cmake/check-version-tag.cmake"
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
        "${CMAKE_SOURCE_DIR}/include"
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

    add_executable(test_netdev_mcast
        test/test_netdev_mcast.c
        netdev_mcast.c)

    target_include_directories(test_netdev_mcast PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_SOURCE_DIR}/include"
        "${CMAKE_SOURCE_DIR}/src/config/test/shim")

    target_compile_options(test_netdev_mcast PRIVATE -Wall -Wextra)

    add_test(NAME netdev_mcast COMMAND test_netdev_mcast)

    # ed.c is #included by the test, not listed here: read_hdr, ring_copy and
    # write_buf are static and the test would otherwise be changing the code
    # it tests.  netdev_bus.c is linked because attach reaches the registers
    # through it.  With no beam on the host, netdev_clock.c preserves the old
    # register-count floor while compiling the measured stop deadline.
    add_executable(test_netdev_ed
        test/test_netdev_ed.c
        netdev_bus.c
        netdev_clock.c
        "${CMAKE_SOURCE_DIR}/src/net68k/n68k_iocopy.c")

    target_include_directories(test_netdev_ed PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_SOURCE_DIR}/include"
        "${CMAKE_SOURCE_DIR}/src/net68k"
        "${CMAKE_SOURCE_DIR}/src/config/test/shim")

    target_compile_options(test_netdev_ed PRIVATE -Wall -Wextra)

    add_test(NAME netdev_ed COMMAND test_netdev_ed)
    add_test(NAME netdev_cards COMMAND test_netdev_cards)

    # netdev_float.h is header-only because both callers of it -- the PCMCIA
    # slot and the ISA PnP bridge -- are files of m68k inline asm that no host
    # can compile.  The guard itself needs neither.
    add_executable(test_netdev_float
        test/test_netdev_float.c)

    target_include_directories(test_netdev_float PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_SOURCE_DIR}/include"
        "${CMAKE_SOURCE_DIR}/src/config/test/shim")

    target_compile_options(test_netdev_float PRIVATE -Wall -Wextra)

    add_test(NAME netdev_float COMMAND test_netdev_float)

    # The PC Card configuration table.  netdev_pcmcia.c is m68k inline asm and
    # Gayle addresses, so the tuple arithmetic was split out to be run here
    # against real cards' bytes -- the CNet CN40BC vector is the CIS the
    # emulator has been replaying at this driver all along.
    add_executable(test_netdev_cis
        test/test_netdev_cis.c
        netdev_cis.c)

    target_include_directories(test_netdev_cis PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_SOURCE_DIR}/include"
        "${CMAKE_SOURCE_DIR}/src/config/test/shim")

    target_compile_options(test_netdev_cis PRIVATE -Wall -Wextra)

    add_test(NAME netdev_cis COMMAND test_netdev_cis)

    # The EtherLink III, which NOTHING EMULATES: Amiberry's PCMCIA support is
    # NE2000 only, so this test is the only thing that has ever driven el3.c.
    # It is a reactive mock rather than a memory image -- a window is not a
    # buffer -- which is why el3.c is #included whole and its raw word access
    # is redirected.  netdev_cards.c is linked for the CIS-to-row selection
    # and netdev_mcaf.c for the multicast hash the part has no hardware for.
    # netdev_clock.c is linked rather than stubbed: on a host it compiles to
    # "there is no beam here", every wait falls back to the caller's own
    # iteration count, and the test drives exactly the loops it drove before
    # the clock existed.  A stub would be a second answer to that question.
    add_executable(test_netdev_el3
        test/test_netdev_el3.c
        netdev_bus.c
        netdev_cards.c
        netdev_clock.c
        netdev_mcaf.c
        netdev_macgen.c
        "${CMAKE_SOURCE_DIR}/src/net68k/n68k_iocopy.c")

    target_include_directories(test_netdev_el3 PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_SOURCE_DIR}/include"
        "${CMAKE_SOURCE_DIR}/src/net68k"
        "${CMAKE_SOURCE_DIR}/src/config/test/shim")

    # The beam netdev_clock.c measures against is supplied by the test, frozen
    # for every case but the late-busy-bit one, which needs a machine whose bus
    # outruns the raster to show a counted bound calling a good card dead.
    target_compile_definitions(test_netdev_el3 PRIVATE NETDEV_CLOCK_TEST=1)
    target_compile_options(test_netdev_el3 PRIVATE -Wall -Wextra)

    add_test(NAME netdev_el3 COMMAND test_netdev_el3)

    # The A2065/Ariadne LANCE.  Its fatal transmit faults are a real-hardware
    # distinction: BUFF/UFLO clear TXON, while emulators normally leave the
    # engine running and never exercise recovery.  A reactive CSR mock drives
    # that stopped state and the descriptor result which caused it.
    add_executable(test_netdev_lance
        test/test_netdev_lance.c)

    target_include_directories(test_netdev_lance PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_SOURCE_DIR}/include"
        "${CMAKE_SOURCE_DIR}/src/net68k"
        "${CMAKE_SOURCE_DIR}/src/config/test/shim")

    target_compile_options(test_netdev_lance PRIVATE -Wall -Wextra)

    add_test(NAME netdev_lance COMMAND test_netdev_lance)

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
        "${CMAKE_SOURCE_DIR}/include"
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
        "${CMAKE_SOURCE_DIR}/include"
        "${CMAKE_SOURCE_DIR}/src/net68k"
        "${CMAKE_SOURCE_DIR}/src/config/test/shim")

    target_compile_options(test_netdev_bus PRIVATE -Wall -Wextra)

    add_test(NAME netdev_bus COMMAND test_netdev_bus)

    # S2_ONEVENT delivery and the S2_PacketFilter hook.  Every one of its
    # callers is an interrupt, so what it does to another task's IORequest is
    # the part no emulator run can show: a list walked without the mask, or a
    # request replied while still linked, is a race and not a failure.  The
    # test drives the list directly and counts the Disable() nesting.
    #
    # Three shim directories, in this order: this one for what only
    # netdev_internal.h needs, tests/sana2's for exec/io.h and the IOSana2Req
    # it is built on, and the config parser's for the rest of exec/.
    add_executable(test_netdev_event
        test/test_netdev_event.c
        netdev_queue.c
        netdev_event.c)

    target_include_directories(test_netdev_event PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_SOURCE_DIR}/include"
        "${CMAKE_SOURCE_DIR}/src/sana2"
        "${CMAKE_CURRENT_SOURCE_DIR}/test/shim"
        "${CMAKE_SOURCE_DIR}/tests/sana2/host/shim"
        "${CMAKE_SOURCE_DIR}/src/config/test/shim")

    target_compile_options(test_netdev_event PRIVATE -Wall -Wextra)

    add_test(NAME netdev_event COMMAND test_netdev_event)

    # The private SANA-II direct-receive handshake.  The EL3 test proves the
    # FIFO half; this proves the device selects exactly one eligible CMD_READ,
    # fills its metadata, completes it once, and puts it back untouched on
    # every decline.  A claim cannot fail after the drain begins, so there is
    # no rollback to test; the declines are the whole restore surface.
    add_executable(test_netdev_direct
        test/test_netdev_direct.c
        netdev_direct.c)

    target_include_directories(test_netdev_direct PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_SOURCE_DIR}/include"
        "${CMAKE_SOURCE_DIR}/src/sana2"
        "${CMAKE_CURRENT_SOURCE_DIR}/test/shim"
        "${CMAKE_SOURCE_DIR}/tests/sana2/host/shim"
        "${CMAKE_SOURCE_DIR}/src/config/test/shim")

    target_compile_options(test_netdev_direct PRIVATE -Wall -Wextra)

    add_test(NAME netdev_direct COMMAND test_netdev_direct)

    add_executable(test_netdev_watchdog
        test/test_netdev_watchdog.c
        netdev_watchdog.c)

    target_include_directories(test_netdev_watchdog PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_SOURCE_DIR}/src/config/test/shim")

    target_compile_options(test_netdev_watchdog PRIVATE -Wall -Wextra)

    add_test(NAME netdev_watchdog COMMAND test_netdev_watchdog)

    # The delay that is measured rather than counted.  NETDEV_CLOCK_TEST swaps
    # the chip read for a beam this test drives, which is the only way to run
    # one binary as a 14 MHz 68020 and as an accelerator and compare what the
    # wait did.  An emulator cannot answer it: every emulator this project
    # runs against emulates a fast machine, so a green run there is consistent
    # with the bug and with the fix alike.
    add_executable(test_netdev_clock
        test/test_netdev_clock.c
        netdev_clock.c)

    target_include_directories(test_netdev_clock PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_SOURCE_DIR}/src/config/test/shim")

    target_compile_definitions(test_netdev_clock PRIVATE NETDEV_CLOCK_TEST=1)
    target_compile_options(test_netdev_clock PRIVATE -Wall -Wextra)

    add_test(NAME netdev_clock COMMAND test_netdev_clock)
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
