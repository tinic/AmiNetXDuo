# anxnet.device and anxgenet.device: the SANA-II drivers, one core.
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
       "Link DEVS:Networks/anxnet.device and anxgenet.device, the SANA-II drivers" ON)

# THE CORE: the AmigaOS device, the SANA-II command table, the queues, the
# events, the probe record, the bus accessors and the address helpers.  Every
# image carries all of it; netdev_roster.h decides at compile time which buses
# it probes and which chip cores it links.
set(NETDEV_CORE_SOURCES
    netdev_device.c     # keep first: romtag, entry guard, Open
    netdev_unit.c       # unit/opener helpers, host-testable
    netdev_io.c         # BeginIO/AbortIO field handling, host-testable
    netdev_direct.c     # claim/completion half of the single-copy RX path
    netdev_verify.c     # stateless IPv4/IPv6 checksum verdict
    netdev_queue.c      # queued IORequest invariants, host-testable
    netdev_cmds.c       # the S2_* table
    netdev_event.c      # S2_ONEVENT delivery and S2_PacketFilter, host-testable
    netdev_watchdog.c   # progress clock for a full transmit ring
    netdev_mcast.c      # exact multicast membership and range transactions
    netdev_bus.c        # register stride and the data port
    netdev_clock.c      # a wait measured against the beam, not counted in reads
    netdev_cards.c      # the card table, rows by roster
    netdev_diag.c       # the probe record, published for CheckNetDevice
    netdev_mcaf.c       # the multicast hash, host-testable
    netdev_macgen.c     # the derived station address, host-testable
    "${CMAKE_SOURCE_DIR}/src/net68k/n68k_iocopy.S"
    "${CMAKE_SOURCE_DIR}/src/net68k/n68k_iocopy.c")

# anxnet.device: every Amiga card.  Zorro boards, the A600/A1200 PCMCIA slot
# and the fixed-address X-Surf 500; the DP8390 family, the LANCE, the
# EtherLink III.
set(NETDEV_CLASSIC_SOURCES
    netdev_pcmcia.c     # the A600/A1200 slot, which has no autoconfig record
    netdev_cis.c        # the PC Card configuration table, host-testable
    netdev_isapnp.c     # the X-Surf's chip, which is behind an ISA PnP bridge
    netdev_cache.c      # a 68030's data cache kept off a Zorro III board
    dp8390.c            # NetBSD chip core
    ne2000.c            # NetBSD remote-DMA glue
    ed.c                # NetBSD mapped-buffer glue: Hydra, ASDG LAN Rover
    lance.c             # Am7990/Am79C960: A2065 and Ariadne
    el3.c)              # 3Com EtherLink III: the 3C589 in the PCMCIA slot

# anxgenet.device: the Raspberry Pi 4 / CM4's own Ethernet behind a PiStorm32
# running Emu68, and nothing else.  Split out on 2026-09-15: the GENET core
# took anxnet.device from 39 KB to 48 KB of RAM on every machine, and most of
# them have no Pi in them.
set(NETDEV_GENET_SOURCES
    genet.c             # Broadcom GENET v5: the Pi 4's MAC behind a PiStorm32
    netdev_dtree.c      # Emu68's device tree and its GIC, for the GENET row
    netdev_reset.c)     # a bus master is stopped before a warm reboot

# anxzz9000.device: the MNT ZZ9000's Ethernet, the Zynq's GEM behind the
# card's ARM firmware.  A Zorro board, so it takes the 68030 cache guard,
# and none of the classic chip cores.
set(NETDEV_ZZ9000_SOURCES
    zz9000.c            # the register block and the two frame windows
    netdev_cache.c)     # a 68030's data cache kept off a Zorro III board

# NETDEV_SOURCES is everything, for the host tests and anything that wants
# the one-image view.
set(NETDEV_SOURCES ${NETDEV_CORE_SOURCES} ${NETDEV_CLASSIC_SOURCES}
                   ${NETDEV_GENET_SOURCES} zz9000.c)

# One SANA-II device image.  `target` is the CMake target, `name` the file
# (and the Exec device name), `roster` the NETDEV_ROSTER_* define, and the
# rest are the sources beyond the core.
function(anxd_add_netdev target name roster)
    add_executable(${target} ${NETDEV_CORE_SOURCES} ${ARGN})
    set_target_properties(${target} PROPERTIES OUTPUT_NAME "${name}")
    target_compile_definitions(${target} PRIVATE
        ${roster}
        "NETDEV_DEVICE_NAME=\"${name}\"")
    target_link_options(${target} PRIVATE "-Wl,-u,_netdev_romtag")
    target_link_options(${target} PRIVATE
        "-Wl,-Map=$<TARGET_FILE_DIR:${target}>/${name}.map")
    target_include_directories(${target} PRIVATE
        ${AMINETXDUO_CORE_INCLUDES}
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_SOURCE_DIR}/src/net68k"   # n68k_iocopy.h, and nothing else
        "${CMAKE_SOURCE_DIR}/src/sana2")   # sana2_device.h, the command numbers
    target_link_options(${target} PRIVATE -nostartfiles -Wl,--gc-sections)
    target_compile_options(${target} PRIVATE -Os)
    if(CMAKE_STRIP AND NOT AMINETXDUO_KEEP_SYMBOLS)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "${CMAKE_STRIP}" "$<TARGET_FILE:${target}>"
            VERBATIM)
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}"
                "-DLIBRARY_BINARY=$<TARGET_FILE:${target}>"
                -P "${CMAKE_SOURCE_DIR}/cmake/check-library-entry.cmake"
        VERBATIM)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}"
                "-DTOOL_BINARY=$<TARGET_FILE:${target}>"
                -P "${CMAKE_SOURCE_DIR}/cmake/check-version-tag.cmake"
        VERBATIM)
    if(AMIGA_SIZE)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "${AMIGA_SIZE}" "$<TARGET_FILE:${target}>"
            COMMENT "${name} size")
    endif()
endfunction()

if(AMINETXDUO_NETDEV_LINK AND CMAKE_CROSSCOMPILING)
    if(AMINETXDUO_NET68K_ASM)
        set_source_files_properties(
            "${CMAKE_SOURCE_DIR}/src/net68k/n68k_iocopy.S"
            "${CMAKE_SOURCE_DIR}/src/net68k/n68k_iocopy.c"
            PROPERTIES COMPILE_DEFINITIONS AMINETXDUO_NET68K_ASM=1)
        set_source_files_properties(
            "${CMAKE_SOURCE_DIR}/src/net68k/n68k_iocopy.S"
            PROPERTIES COMPILE_OPTIONS "${AMIGA_ARCH_FLAGS}")
    endif()

    # The file and Exec name of the classic image.  A candidate under another
    # name can be opened beside the installed anxnet.device on a machine
    # whose only network is that card: an interface file naming
    # DEVICE=anxtest.device loads it while the shipped one stays resident.
    set(AMINETXDUO_ANXNET_NAME "anxnet.device" CACHE STRING
        "File and Exec device name of the classic-card SANA-II image")
    anxd_add_netdev(anxnet_device   ${AMINETXDUO_ANXNET_NAME}
                    NETDEV_ROSTER_CLASSIC ${NETDEV_CLASSIC_SOURCES})
    anxd_add_netdev(anxgenet_device anxgenet.device NETDEV_ROSTER_GENET
                    ${NETDEV_GENET_SOURCES})
    anxd_add_netdev(anxzz9000_device anxzz9000.device NETDEV_ROSTER_ZZ9000
                    ${NETDEV_ZZ9000_SOURCES})
endif()

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

    # The device-tree reader over a tree built in memory: the eleven
    # devicetree.resource calls are the seam, supplied by the test.
    add_executable(test_netdev_dtree
        test/test_netdev_dtree.c
        netdev_dtree.c)
    target_compile_definitions(test_netdev_dtree PRIVATE NETDEV_DTREE_TEST=1)
    target_include_directories(test_netdev_dtree PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/test/shim"
        "${CMAKE_SOURCE_DIR}/src/config/test/shim")
    target_compile_options(test_netdev_dtree PRIVATE -Wall -Wextra)
    add_test(NAME netdev_dtree COMMAND test_netdev_dtree)

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
        netdev_verify.c
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

    # The accessors the DEVICE runs.  The test above replaces LANCE_CSR_GET
    # and LANCE_CSR_PUT with a mock, so everything behind lance.c's
    # `#ifndef LANCE_CSR_GET` -- the RAP/RDP pair and the select cache -- is
    # compiled out of it and had no coverage anywhere.  This one includes
    # lance.c without the override.
    add_executable(test_netdev_lance_csr
        test/test_netdev_lance_csr.c)

    target_include_directories(test_netdev_lance_csr PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_SOURCE_DIR}/include"
        "${CMAKE_SOURCE_DIR}/src/net68k"
        "${CMAKE_SOURCE_DIR}/src/config/test/shim")

    target_compile_options(test_netdev_lance_csr PRIVATE -Wall -Wextra)

    add_test(NAME netdev_lance_csr COMMAND test_netdev_lance_csr)

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

    # The write side needs the seam, so it INCLUDES netdev_bus.c rather than
    # linking it: the data port is one address and a linked build can observe
    # only the last value written, never the order or the count.  n68k_iocopy.c
    # is still linked for the read-path primitives the file does not seam.
    add_executable(test_netdev_buswrite
        test/test_netdev_buswrite.c
        "${CMAKE_SOURCE_DIR}/src/net68k/n68k_iocopy.c")

    target_include_directories(test_netdev_buswrite PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_SOURCE_DIR}/include"
        "${CMAKE_SOURCE_DIR}/src/net68k"
        "${CMAKE_SOURCE_DIR}/src/config/test/shim")

    target_compile_options(test_netdev_buswrite PRIVATE -Wall -Wextra)

    add_test(NAME netdev_buswrite COMMAND test_netdev_buswrite)

    # The NE2000 probe ORDER, which is the half test_netdev_bus.c cannot see.
    # ne2000_detect() demands the command register read back as a reset chip
    # before it ever asks whether the odd registers can be read, and the reset
    # port is odd and strobed by READING it -- so on a clone that answers only
    # 16-bit cycles the card was refused before the probe written for it ran.
    # Amiberry decodes both PCMCIA windows 1:1 and cannot hold a card that
    # refuses a byte read, so the retry never fires there and no emulator arm
    # can cover this. ne2000.c is #included whole and its four register
    # accessors are replaced: the chip is indexed by register number, because
    # the word read keeping an odd register in its low half is a big-endian
    # fact that no little-endian host can hold beside the byte read of the
    # even register next to it.
    add_executable(test_netdev_ne2000
        test/test_netdev_ne2000.c
        netdev_bus.c
        netdev_cards.c
        netdev_clock.c
        netdev_mcaf.c
        netdev_macgen.c
        "${CMAKE_SOURCE_DIR}/src/net68k/n68k_iocopy.c")

    target_include_directories(test_netdev_ne2000 PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_SOURCE_DIR}/include"
        "${CMAKE_SOURCE_DIR}/src/net68k"
        "${CMAKE_SOURCE_DIR}/src/config/test/shim")

    # ne2000.c puts a register window pointer in a ULONG for the probe record,
    # which is exact on the m68k this file is written for and narrowing on a
    # 64-bit host.  The cast is not the test's to fix and the value is only
    # ever printed, so the warning is off for this target alone.
    target_compile_options(test_netdev_ne2000 PRIVATE
        -Wall -Wextra -Wno-pointer-to-int-cast)

    add_test(NAME netdev_ne2000 COMMAND test_netdev_ne2000)

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

    # What an opener's filter state and its departure do to a unit.  These
    # three lived in netdev_device.c, which cannot be compiled off target --
    # a raw romtag asm at its head and <exec/execbase.h> at :19 -- so none of
    # them had ever run on a host.  netdev_unit.c is the split that let them,
    # at a MEASURED 4 bytes of resident RAM (38,832 -> 38,836 with -flto,
    # budget 40,000), and the same 4 whether one function moves or all three,
    # because the cost is the extra translation unit and not its contents.
    #
    # The hash is why it was worth 4 bytes.  netdev_rebuild_filter() builds
    # the 64-bit multicast filter every core programs into its chip, and a bit
    # not set there means the card drops the router's neighbour solicitation
    # to 33:33:ff:xx:xx:xx -- on-link IPv6 works, off-link never resolves.
    # emulator.yml:466-470 records exactly that happening to three cards.
    add_executable(test_netdev_unit
        test/test_netdev_unit.c
        netdev_unit.c
        netdev_mcaf.c)

    target_include_directories(test_netdev_unit PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_SOURCE_DIR}/include"
        "${CMAKE_SOURCE_DIR}/src/sana2"
        "${CMAKE_CURRENT_SOURCE_DIR}/test/shim"
        "${CMAKE_SOURCE_DIR}/tests/sana2/host/shim"
        "${CMAKE_SOURCE_DIR}/src/config/test/shim")

    target_compile_options(test_netdev_unit PRIVATE -Wall -Wextra)

    add_test(NAME netdev_unit COMMAND test_netdev_unit)

    # The probe record and the semaphore it is published on -- the ONLY channel
    # by which a machine nobody here has can report what the probe decided.
    # Three card rows are marked UNVERIFIED ON HARDWARE in netdev_cards.c, the
    # X-Surf 500 has no emulator at all, and the ISA PnP sequence records its
    # vendor id, serial, checksum and settle rounds here and nowhere else.
    #
    # The publish path is the half worth a test: a SECOND anxnet.device on one
    # machine has to find the first one's record and leave it alone, and its
    # expunge must not RemSemaphore() a semaphore it never added.  The exec
    # stubs keep a real list and fail an Add or Rem outside Forbid().
    add_executable(test_netdev_diag
        test/test_netdev_diag.c
        netdev_diag.c
        netdev_cards.c)

    target_include_directories(test_netdev_diag PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_SOURCE_DIR}/include"
        "${CMAKE_SOURCE_DIR}/src/sana2"
        "${CMAKE_CURRENT_SOURCE_DIR}/test/shim"
        "${CMAKE_SOURCE_DIR}/tests/sana2/host/shim"
        "${CMAKE_SOURCE_DIR}/src/config/test/shim")

    target_compile_options(test_netdev_diag PRIVATE -Wall -Wextra)

    add_test(NAME netdev_diag COMMAND test_netdev_diag)

    # The DP8390 core, beneath ne2000.c AND ed.c and therefore beneath six of
    # the nine cards the sweep drives: X-Surf, X-Surf 100, Ariadne II, Hydra,
    # LAN Rover and the PCMCIA slot.  It had NO host test, and the way that
    # was missed is worth writing down: test_netdev_ne2000.c and
    # test_netdev_ed.c cover their card glue by #including it whole and STUB
    # all seven entry points of the core underneath (:133-140 and :33-37), so
    # the two tests that look like they drive a DP8390 are the two that
    # guarantee they do not.
    #
    # No other source: NIC_GET/NIC_PUT are redirected to a model that BANKS
    # the register file.  A DP8390 is four pages of sixteen selected by CR
    # bits 7..6, so PAR0 on page 1 and PSTART on page 0 are one register
    # index -- an array fuses them and a driver that forgot to select page 1
    # before writing the station address would still pass.
    add_executable(test_netdev_dp8390
        test/test_netdev_dp8390.c
        netdev_verify.c)

    target_include_directories(test_netdev_dp8390 PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_SOURCE_DIR}/include"
        "${CMAKE_SOURCE_DIR}/src/sana2"
        "${CMAKE_SOURCE_DIR}/src/net68k"
        "${CMAKE_CURRENT_SOURCE_DIR}/test/shim"
        "${CMAKE_SOURCE_DIR}/tests/sana2/host/shim"
        "${CMAKE_SOURCE_DIR}/src/config/test/shim")

    target_compile_options(test_netdev_dp8390 PRIVATE -Wall -Wextra)

    add_test(NAME netdev_dp8390 COMMAND test_netdev_dp8390)

    # The ISA Plug and Play sequence in front of an X-Surf's RTL8019AS, the
    # last file in this directory a host could compile and nothing did.
    #
    # The emulated xsurf arm does NOT cover it in the way that sounds: it
    # proves the card came up, so it proves the sequence works against
    # Amiberry's model of the board.  It says nothing about which byte went to
    # which address -- and the ADDRESS and WRITE_DATA ports are ONE board
    # address with a latch bit between them, so a sequence that sent the whole
    # initiation key to the wrong one still produces a plausible log.
    #
    # netdev_cards.c is linked for real, not stood in for: the row is where a
    # card is added, and the two numbers its comment states -- both ports at
    # board+$84f2, and (reg_off - io_win) / stride == ISA port $300 -- are
    # asserted against it rather than against a fixture.
    add_executable(test_netdev_isapnp
        test/test_netdev_isapnp.c
        netdev_cards.c)

    target_include_directories(test_netdev_isapnp PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_SOURCE_DIR}/include"
        "${CMAKE_SOURCE_DIR}/src/sana2"
        "${CMAKE_CURRENT_SOURCE_DIR}/test/shim"
        "${CMAKE_SOURCE_DIR}/tests/sana2/host/shim"
        "${CMAKE_SOURCE_DIR}/src/config/test/shim")

    target_compile_options(test_netdev_isapnp PRIVATE -Wall -Wextra)

    add_test(NAME netdev_isapnp COMMAND test_netdev_isapnp)

    # The 68030 data-cache guard for a Zorro III board, its six machine seams
    # and the core's coherence question supplied by the test.  netdev_cache.c
    # is #included under NETDEV_CACHE_TEST for the seams.
    add_executable(test_netdev_cache
        test/test_netdev_cache.c)

    target_include_directories(test_netdev_cache PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_SOURCE_DIR}/include"
        "${CMAKE_SOURCE_DIR}/src/sana2"
        "${CMAKE_CURRENT_SOURCE_DIR}/test/shim"
        "${CMAKE_SOURCE_DIR}/tests/sana2/host/shim"
        "${CMAKE_SOURCE_DIR}/src/config/test/shim")

    target_compile_options(test_netdev_cache PRIVATE -Wall -Wextra)

    add_test(NAME netdev_cache COMMAND test_netdev_cache)

    # The SANA-II command table itself, 818 lines that nothing compiled
    # off-target until now: test_netdev_beginio below REPLACES netdev_cmds.c
    # with a recorder, because that test is about what BeginIO writes before
    # dispatch.  So the twenty cases in it had the coverage an emulated a2065
    # run gives them -- CMD_READ, CMD_WRITE, S2_CONFIGINTERFACE, S2_ONLINE --
    # and every error path was unasserted.  The error codes ARE the contract:
    # they are the ones Individual Computers' x-surf.device returns.
    #
    # netdev_mcast.c is linked for real rather than stood in for: the group
    # table and its refcounts are what several of the cases turn on, and the
    # deliberate divergence from both IC drivers -- accepting a join whose
    # group bit is set, where they test bit 7 and so refuse every real IPv4
    # and IPv6 multicast address -- is asserted through it in both directions.
    add_executable(test_netdev_cmds
        test/test_netdev_cmds.c
        netdev_cmds.c
        netdev_mcast.c)

    target_include_directories(test_netdev_cmds PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_SOURCE_DIR}/include"
        "${CMAKE_SOURCE_DIR}/src/sana2"
        "${CMAKE_CURRENT_SOURCE_DIR}/test/shim"
        "${CMAKE_SOURCE_DIR}/tests/sana2/host/shim"
        "${CMAKE_SOURCE_DIR}/src/config/test/shim")

    target_compile_options(test_netdev_cmds PRIVATE -Wall -Wextra)

    add_test(NAME netdev_cmds COMMAND test_netdev_cmds)

    # What BeginIO does to a request before it dispatches it.  netdev_event.c
    # is driven directly by the test above, which is why it stayed green over
    # a BeginIO that cleared ios2_WireError for every command including
    # S2_ONEVENT, whose event mask arrives IN that field: every event request
    # this device ever received named no condition and was refused.  This
    # enters through the real netdev_begin_io() with netdev_cmds.c replaced by
    # a recorder, and asserts the general rule -- io_Error, ios2_WireError
    # where it is an output, and not one byte more.
    add_executable(test_netdev_beginio
        test/test_netdev_beginio.c
        netdev_io.c)

    target_include_directories(test_netdev_beginio PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_SOURCE_DIR}/include"
        "${CMAKE_SOURCE_DIR}/src/sana2"
        "${CMAKE_CURRENT_SOURCE_DIR}/test/shim"
        "${CMAKE_SOURCE_DIR}/tests/sana2/host/shim"
        "${CMAKE_SOURCE_DIR}/src/config/test/shim")

    target_compile_options(test_netdev_beginio PRIVATE -Wall -Wextra)

    add_test(NAME netdev_beginio COMMAND test_netdev_beginio)

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

    add_executable(test_netdev_verify
        test/test_netdev_verify.c
        netdev_verify.c)

    target_include_directories(test_netdev_verify PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_SOURCE_DIR}/include"
        "${CMAKE_SOURCE_DIR}/src/config/test/shim")

    target_compile_options(test_netdev_verify PRIVATE -Wall -Wextra)

    add_test(NAME netdev_verify COMMAND test_netdev_verify)

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
    # NETDEV_TIME alone.  It used to imply NETDEV_TRACE, which prints a line
    # per COMMAND -- one per received frame -- and raw serial under this
    # emulator is slow enough that the trace throttled the guest to 76 frames a
    # second against 480.  Every span the instrument reported was then measured
    # on a machine the instrument had slowed by six times.  The report's own
    # output goes through the same serial path but runs once per 512 frames.
    target_compile_definitions(anxnet_device PRIVATE NETDEV_TIME=1)
endif()

option(AMINETXDUO_NETDEV_TRACE "Trace anxnet.device to the serial port" OFF)
if(AMINETXDUO_NETDEV_TRACE AND TARGET anxnet_device)
    target_compile_definitions(anxnet_device PRIVATE NETDEV_TRACE=1)
endif()
