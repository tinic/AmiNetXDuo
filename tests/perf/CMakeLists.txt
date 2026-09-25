# The TCP data-path cost census.
#
#   cmake -S . -B build/cm -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-m68k-amigaos.cmake \
#         -DCMAKE_BUILD_TYPE=Release
#   cmake --build build/cm --parallel --target perf_test
#
#   AMINETXDUO_RUN_TAG=perf20 ./tools/amiberry-run.sh -t 900 build/cm/tests/perf/perf_test
#
# To price the C library's memcpy rather than net68k's, configure a second tree
# with -DAMINETXDUO_NET68K_MEMCPY=OFF: with the hook on, memcpy() resolves to
# n68k_copy_bytes() and the "memcpy" rows measure it under a second name.
#
# Only the 68020 profiles are meaningful.  FS-UAE switches cycle accounting off
# for every CPU above a 68020, so the 68030 charges nothing for anything; the
# cpucal target below measures that rather than asserting it, and
# docs/RESEARCH.md writes it up.  To ask what a faster machine would do, move
# the 68020 model's clock instead:
#
#   AMINETXDUO_RUN_TAG=perf25 ./tools/amiberry-run.sh -t 1200 -k 25 \
#       build/cm/tests/perf/perf_test
#
# SPDX-License-Identifier: MIT

# stallgaps.py's ACK-clock attribution: whether the acknowledgement that ended
# a progress gap was the thing that ended it.  Pure arithmetic over synthetic
# segments, so it needs no capture, no emulator and no network, and it is the
# only thing in the tree that has an opinion about what those numbers mean.
# Recovered with the functions it covers; both were lost in a rewrite.
if(NOT CMAKE_CROSSCOMPILING)
    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME perf_stallgaps
                 COMMAND "${Python3_EXECUTABLE}"
                         "${CMAKE_CURRENT_SOURCE_DIR}/test_stallgaps.py")

        # prof-report.py had no coverage at all, which is how it shipped a
        # symbol table that put every symbol of an object at every address the
        # object contributed to.  The profile named a rare trim path at 2.4%
        # and left the per-segment function out; the campaign chased duplicate
        # data on the strength of it.  Hermetic -- a synthetic map and a
        # stubbed nm, no toolchain and no build.
        add_test(NAME perf_prof_report
                 COMMAND "${Python3_EXECUTABLE}"
                         "${CMAKE_CURRENT_SOURCE_DIR}/test_prof_report.py")

        # profreport.py, the tree's own copy of the same parser, had the bug
        # again as a collapse (base_section) instead of a bare-name match:
        # it mapped .text.<name> to .text before the suffix could name the
        # function, so every symbol of an object landed at every address the
        # object contributed to.  Two .text.<fn> sections, both nm values
        # zero, is the smallest case that shows it.
        add_test(NAME perf_profreport
                 COMMAND "${Python3_EXECUTABLE}"
                         "${CMAKE_CURRENT_SOURCE_DIR}/test_profreport.py")

        # The Roadshow RX-gap verdict (gap / no material gap / inconclusive,
        # docs/plans/roadshow-rx-gap.md) on synthetic boots, and the peer
        # sender's loopback selftest.
        add_test(NAME perf_rsgap
                 COMMAND "${Python3_EXECUTABLE}"
                         "${CMAKE_CURRENT_SOURCE_DIR}/test_rsgap.py")
    endif()
endif()

# perf_test defines _nx_ip_checksum_compute() itself, so it can only be built
# when the vendored object is out of the core, i.e. when the replacement is
# switched on.  With -DAMINETXDUO_NET68K_CHECKSUM=OFF the two definitions would
# collide, and the A/B it exists to run would be meaningless anyway.
if(CMAKE_CROSSCOMPILING AND AMINETXDUO_NET68K_CHECKSUM)

# The mode lister and the colour bars, the two halves of proving a truecolour
# readback.  rtgmodes says what the board really publishes, so a mode ID is
# read off the machine instead of guessed at; rtgbars puts a picture with a
# known red and a known blue in front of the console, so a readback that
# transposes them fails instead of looking deliberate.  Neither ships.
#
# STARTED LIKE A COMMAND, NOT LIKE A TEST.  Both run from S:Startup-Sequence on
# the console harness's RTG guest, and their predecessor -- rtgscreen, which
# these two replace -- did not survive that position: linked against the
# toolchain crt0 and printing through newlib, it died at a garbage PC in low
# memory before its first line reached the file it was redirected into, taking
# the boot and the server three lines below it with it.  So they link
# src/tools/tool_startup.S under -nostartfiles and print with VPrintf(), which
# is what every command that demonstrably runs on that guest does.
# tool_startup.S FIRST in each list: AmigaOS enters a command at the first byte
# of the first hunk and the linker script carries no ENTRY(), so *(.text) is
# filled in link order.  src/tools/CMakeLists.txt says the same thing and checks
# its map for it.
# tool_startup.S calls the stack probe around main() when AMINETXDUO_STACKPROBE
# is on, so anything that uses the stub needs the probe beside it or the link
# fails on an undefined reference.  These three take the stub without
# TOOLS_COMMON_SOURCES, which is where the commands get it.
set(RTG_STACKPROBE "")
if(AMINETXDUO_STACKPROBE)
    set(RTG_STACKPROBE "${CMAKE_SOURCE_DIR}/src/tools/tool_stackprobe.c")
endif()

add_executable(rtgmodes "${CMAKE_SOURCE_DIR}/src/tools/tool_startup.S" rtgmodes.c ${RTG_STACKPROBE})
add_executable(rtgbars "${CMAKE_SOURCE_DIR}/src/tools/tool_startup.S" rtgbars.c ${RTG_STACKPROBE})

foreach(rtgtool rtgmodes rtgbars)
    target_include_directories(${rtgtool} PRIVATE ${AMINETXDUO_CORE_INCLUDES})
    if(AMIGA_NDK_INCLUDE)
        target_include_directories(${rtgtool} PRIVATE "${AMIGA_NDK_INCLUDE}")
    endif()
    target_compile_options(${rtgtool} PRIVATE -Wall -Wextra)
    target_link_options(${rtgtool} PRIVATE -nostartfiles)
endforeach()

# What libgcc's 64-bit helpers cost against src/common/ami_udivdi3.c's, priced
# in the guest.  Two links of one source; see the head of rtdiv.c.
if(CMAKE_CROSSCOMPILING AND TARGET aminetxduo_m68k_rt)
    foreach(rtdivarm rtdiv_libgcc rtdiv_anxd)
        add_executable(${rtdivarm}
            "${CMAKE_SOURCE_DIR}/src/tools/tool_startup.S" rtdiv.c
            ${RTG_STACKPROBE})
        target_include_directories(${rtdivarm} PRIVATE ${AMINETXDUO_CORE_INCLUDES})
        if(AMIGA_NDK_INCLUDE)
            target_include_directories(${rtdivarm} PRIVATE "${AMIGA_NDK_INCLUDE}")
        endif()
        target_compile_options(${rtdivarm} PRIVATE -Wall -Wextra)
        target_link_libraries(${rtdivarm} PRIVATE aminetxduo_common)
        target_link_options(${rtdivarm} PRIVATE -nostartfiles -Wl,--gc-sections
            "-Wl,-Map=$<TARGET_FILE_DIR:${rtdivarm}>/${rtdivarm}.map")
        set_target_properties(${rtdivarm} PROPERTIES OUTPUT_NAME "${rtdivarm}")
    endforeach()

    target_link_libraries(rtdiv_anxd PRIVATE aminetxduo_m68k_rt)
    target_compile_definitions(rtdiv_anxd PRIVATE AMINETXDUO_TOOL_RT)
endif()

# The chipset screen prober: a public HAM6, HAM8 or extra half-brite screen
# with a deterministic ramp on it, left in front for the console to serve.  A
# TEST TOOL and nothing ships it; see the file for why a photograph on the
# screen would prove nothing about a HAM decode.
add_executable(chipscreen chipscreen.c)
target_include_directories(chipscreen PRIVATE ${AMINETXDUO_CORE_INCLUDES})
if(AMIGA_NDK_INCLUDE)
    target_include_directories(chipscreen PRIVATE "${AMIGA_NDK_INCLUDE}")
endif()
target_compile_options(chipscreen PRIVATE -Wall -Wextra)

add_executable(perf_test perf_test.c)

# aminetxduo_net68k for the algorithm, and NOT its hook: perf_test.c defines
# _nx_ip_checksum_compute() itself so it can dispatch between the vendored
# implementation and ours at run time, with nothing else in the process
# changing.  The vendored file is compiled in here under a renamed symbol --
# the top-level CMakeLists dropped it from the NetX Duo glob.
add_library(perf_checksum_ref STATIC
    "${AMINETXDUO_NETXDUO}/common/src/nx_ip_checksum_compute.c")
target_include_directories(perf_checksum_ref PUBLIC ${AMINETXDUO_CORE_INCLUDES})
target_compile_definitions(perf_checksum_ref PRIVATE
    _nx_ip_checksum_compute=n68k_checksum_reference)

# aminetxduo_sana2 is here for exactly one object, sana2_copy.c, whose
# ami_sana2_copy_bytes() is the copy loop the RX interrupt runs.  Archive
# semantics pull that member and nothing else.
target_link_libraries(perf_test PRIVATE
    perf_checksum_ref aminetxduo_net68k netxduo_port
    aminetxduo_sana2 aminetxduo_common)

if(AMIGA_NDK_INCLUDE)
    target_include_directories(perf_test PRIVATE "${AMIGA_NDK_INCLUDE}")
endif()

set_target_properties(perf_test PROPERTIES OUTPUT_NAME "perf_test")

if(AMIGA_SIZE)
    add_custom_command(TARGET perf_test POST_BUILD
        COMMAND "${AMIGA_SIZE}" "$<TARGET_FILE:perf_test>"
        COMMENT "perf_test size")
endif()

endif() # CMAKE_CROSSCOMPILING


# ------------------------------------------------------------- FitzBench --
#
# Bulk file throughput over a mounted share.  Links against nothing of ours --
# it is AmigaDOS Read()/Write() and timer.device, so the library under test
# is whatever is in LIBS:, which is what makes an A/B of two builds possible
# without rebuilding the benchmark.
#
#   cmake --build build/cm --parallel --target fitzbench
#   tests/perf/run-fitzbench.sh -H 192.168.1.184

if(CMAKE_CROSSCOMPILING)

    add_executable(fitzbench fitzbench.c)
    set_target_properties(fitzbench PROPERTIES OUTPUT_NAME "FitzBench")

    # A linker map, because this is the program tests/perf/run-stackprof.sh
    # points tools/profiler/Profile at, and profreport.py cannot name a
    # file-static without one: the linker drops those from HUNK_SYMBOL and the
    # only route back is where the map placed each object plus nm on it.
    # Without it every sample in a static lands silently on the preceding
    # global, and the ranking looks plausible and is wrong.
    target_link_options(fitzbench PRIVATE
        "-Wl,-Map=$<TARGET_FILE_DIR:fitzbench>/FitzBench.map")

    if(AMIGA_NDK_INCLUDE)
        target_include_directories(fitzbench PRIVATE "${AMIGA_NDK_INCLUDE}")
    endif()

endif()


# ------------------------------------------------------ Roadshow RX gap --
#
# docs/plans/roadshow-rx-gap.md.  RsGapRx is the one receive client both arms
# run: plain bsdsocket.library by name, nothing of ours linked, so LIBS:
# decides the stack.  RsGapBoot is the stack-neutral selector, watchdog and
# recorder.  tests/perf/run-rsgap.sh stages and drives both.
#
#   cmake --build build/cm --parallel --target rsgaprx rsgapboot

if(CMAKE_CROSSCOMPILING)
    add_executable(rsgaprx rsgaprx.c)
    set_target_properties(rsgaprx PROPERTIES OUTPUT_NAME "RsGapRx")
    add_executable(rsgapboot rsgapboot.c)
    set_target_properties(rsgapboot PROPERTIES OUTPUT_NAME "RsGapBoot")
    foreach(rsgaptool rsgaprx rsgapboot)
        target_compile_options(${rsgaptool} PRIVATE -Wall -Wextra)
        if(AMIGA_NDK_INCLUDE)
            target_include_directories(${rsgaptool} PRIVATE "${AMIGA_NDK_INCLUDE}")
        endif()
    endforeach()
endif()


# ------------------------------------------------------- the bracket probe --
#
# What one tx_amiga_adopt_thread()/tx_amiga_orphan_thread() pair costs, and
# what a recv() costs with and without one.  Unlike perf_test above it needs
# nothing of net68k's checksum hook, so it builds on every profile.
#
#   cmake --build build/cm --parallel --target bracket_test
#   AMINETXDUO_RUN_TAG=brk ./tools/amiberry-run.sh -t 900 -m A1200 \
#       build/cm/tests/perf/bracket_test
#
# Read the header of bracket_test.c before quoting anything it prints, and
# only the 68020 profiles mean anything, for the reason cpucal.c measures.

if(CMAKE_CROSSCOMPILING)

    add_executable(bracket_test bracket_test.c)
    target_link_libraries(bracket_test PRIVATE netxduo_port aminetxduo_common)
    set_target_properties(bracket_test PROPERTIES OUTPUT_NAME "bracket_test")

    if(AMIGA_NDK_INCLUDE)
        target_include_directories(bracket_test PRIVATE "${AMIGA_NDK_INCLUDE}")
    endif()

    if(AMIGA_SIZE)
        add_custom_command(TARGET bracket_test POST_BUILD
            COMMAND "${AMIGA_SIZE}" "$<TARGET_FILE:bracket_test>"
            COMMENT "bracket_test size")
    endif()

endif()


# ------------------------------------------------------ the variant probe --
#
# n68kmv checks and times every multiversioned inner loop against the others
# on the machine it is run on, so it exists only where there is more than one
# of them.  It is also the only instrument that can settle the two 68060 bets
# in n68k_copy.S and n68k_checksum.S, which are reasoning and not measurement
# because no emulator charges cycles above a 68020.
#
#   cmake --build build/any --parallel --target n68kmv
#   AMINETXDUO_RUN_TAG=mv ./tools/amiberry-run.sh -t 300 -m A1200 \
#       build/any/tests/perf/n68kmv

if(CMAKE_CROSSCOMPILING AND AMINETXDUO_NET68K_ASM AND
   AMINETXDUO_CPU STREQUAL "any")

    add_executable(n68kmv n68kmv.c)
    target_link_libraries(n68kmv PRIVATE aminetxduo_net68k aminetxduo_common)
    set_target_properties(n68kmv PROPERTIES OUTPUT_NAME "n68kmv")

    if(AMIGA_NDK_INCLUDE)
        target_include_directories(n68kmv PRIVATE "${AMIGA_NDK_INCLUDE}")
    endif()

endif()


# ------------------------------------------------------- calibration probe --
#
# cpucal answers the question every other number in this directory depends on:
# what does the emulator actually charge for an instruction?  It has no
# dependency on NetX Duo at all, so it builds whatever the net68k options say
# including on a profile where perf_test above is switched off.
#
#   cmake --build build/cm --parallel --target cpucal
#   AMINETXDUO_RUN_TAG=cal ./tools/amiberry-run.sh -t 300 -m A1200 build/cm/tests/perf/cpucal
#   AMINETXDUO_RUN_TAG=cal ./tools/amiberry-run.sh -t 300 -m A3000 build/cm/tests/perf/cpucal
#
# Read the header of cpucal.c before quoting anything it prints.

# 68020 AND UP ONLY, and not for the usual "we did not get round to it"
# reason: cpucal exists to time MULU.L against the MC68020UM's published
# figures, so on a 68000, which has no MULU.L, there is no measurement for
# it to make.  Excluded rather than made portable.  docs/RESEARCH.md §45.
#
# `any` is excluded on the same rule.  It compiles -m68000 because it has to
# run on one, so the flag would reject the file, and a tree that does not know
# its CPU has nothing to calibrate: configure a second tree with
# -DAMINETXDUO_CPU=68020 to ask this question.
if(CMAKE_CROSSCOMPILING AND NOT AMINETXDUO_CPU MATCHES "^(68000|any)$")

    add_executable(cpucal cpucal.c cpucal.S)
    target_link_libraries(cpucal PRIVATE aminetxduo_common)
    set_target_properties(cpucal PROPERTIES OUTPUT_NAME "cpucal")

    # The assembler defaults to a plain 68000 and rejects MULU.L outright --
    # the same default that made objdump print `.short 0x4c06` in the crypto
    # work.  src/net68k/ and src/crypto68k/ pass the arch flags to their .S
    # files for the same reason.
    set_source_files_properties(cpucal.S PROPERTIES
        COMPILE_OPTIONS "${AMIGA_ARCH_FLAGS}")

    if(AMIGA_NDK_INCLUDE)
        target_include_directories(cpucal PRIVATE "${AMIGA_NDK_INCLUDE}")
    endif()

endif()


# ---------------------------------------------------------------- host tier --
#
# The differential check for the checksum replacement, registered with ctest so
# it runs on every push.  See host/shim/tx_port.h for why a shim is needed and
# what the host tier therefore cannot cover (the assembly).

if(NOT CMAKE_CROSSCOMPILING)

    add_executable(test_net68k_checksum
        host/test_checksum_host.c
        "${CMAKE_SOURCE_DIR}/src/net68k/n68k_checksum.c"
        "${AMINETXDUO_NETXDUO}/common/src/nx_ip_checksum_compute.c")

    target_include_directories(test_net68k_checksum PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/host/shim"     # tx_port.h, first
        "${AMINETXDUO_NETXDUO}/ports/linux/gnu/inc" # nx_port.h
        "${AMINETXDUO_NETXDUO}/common/inc"
        "${AMINETXDUO_THREADX}/common/inc"
        "${CMAKE_SOURCE_DIR}/src/net68k")

    target_compile_definitions(test_net68k_checksum PRIVATE
        AMINETXDUO_HOST_VENDORED_TX_PORT="${AMINETXDUO_THREADX}/ports/linux/gnu/inc/tx_port.h"
        _nx_ip_checksum_compute=n68k_checksum_reference)

    # The differential runs both implementations at every start alignment, odd
    # ones included.  Ours carries N68K_NO_SANITIZE_ALIGNMENT per function; the
    # vendored reference cannot (submodule), so UBSan's alignment check alone
    # is dropped for that one file, in this test target only.
    set_source_files_properties(
        "${AMINETXDUO_NETXDUO}/common/src/nx_ip_checksum_compute.c"
        PROPERTIES COMPILE_OPTIONS "-fno-sanitize=alignment")

    # The receive verifier, on the host, because its whole job is a decision
    # about an IPv4 frame in memory and that is steppable here and not in an
    # emulator.
    add_executable(test_net68k_rxverify
        host/test_rxverify_host.c
        "${CMAKE_SOURCE_DIR}/src/net68k/n68k_rx_verify.c"
        "${CMAKE_SOURCE_DIR}/src/net68k/n68k_checksum.c")

    target_include_directories(test_net68k_rxverify PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/host/shim"
        "${AMINETXDUO_NETXDUO}/ports/linux/gnu/inc"
        "${AMINETXDUO_NETXDUO}/common/inc"
        "${AMINETXDUO_THREADX}/common/inc"
        "${CMAKE_SOURCE_DIR}/src/net68k")

    # FEATURE_NX_IPV6 is what selects the 128-bit pseudo header in the checksum
    # function, so the IPv6 half of the verifier only exists with it on.  The
    # noipv6 cross arm is what compiles the other shape.
    target_compile_definitions(test_net68k_rxverify PRIVATE
        AMINETXDUO_RX_VERIFY
        AMINETXDUO_RX_VERIFY_STATS
        FEATURE_NX_IPV6
        NX_ENABLE_INTERFACE_CAPABILITY
        AMINETXDUO_HOST_VENDORED_TX_PORT="${AMINETXDUO_THREADX}/ports/linux/gnu/inc/tx_port.h"
        _nx_ip_checksum_compute=n68k_ip_checksum_compute)

    include(CTest)
    add_test(NAME net68k_rxverify COMMAND test_net68k_rxverify)
    add_test(NAME net68k_checksum COMMAND test_net68k_checksum)

endif()


# -------------------------------------------------- the encoder profile ----
#
# Where /console's frame encoder spends its time, measured on the machine.
#
#   cmake --build build/cm --parallel --target rfbprof
#   AMINETXDUO_RUN_TAG=rfb ./tools/amiberry-run.sh -t 600 -m A1200 \
#       -a DH0:d2-idle.pfs build/cm/tests/perf/rfbprof <capture>.pfs
#
# rfb_encode.c is compiled TWICE into it.  aminetxduo_rfb is the shipping
# object, which on AMINETXDUO_CPU=any is -m68000; rfb_encode_020 is the same
# source at -m68020 with its public symbols renamed, so one run times
# both codegens over the same bytes in the same second.  That is the only way
# to answer "what is the -m68000 codegen costing the hottest loop in the
# program" without comparing two runs on a shared host.

if(CMAKE_CROSSCOMPILING)

    # Every public function of the encoder, read off its header, so that a
    # second copy of the source under another prefix renames ALL of them.
    # The list was typed out three times here and a new pair
    # (rfb_packbits16) missed every copy: the default cross arm then failed
    # to link rfbprof, on the runner, after the host stage had passed.
    file(STRINGS "${CMAKE_SOURCE_DIR}/include/aminetxduo/rfb_encode.h"
         _rfb_decls REGEX "^[a-z0-9_]+ +rfb_[a-z0-9_]+\\(")
    set(RFB_EXPORTS "")
    foreach(_d IN LISTS _rfb_decls)
        string(REGEX REPLACE "^[a-z0-9_]+ +(rfb_[a-z0-9_]+)\\(.*$" "\\1"
               _n "${_d}")
        list(APPEND RFB_EXPORTS "${_n}")
    endforeach()
    list(LENGTH RFB_EXPORTS _rfb_n)
    if(_rfb_n LESS 12)
        message(FATAL_ERROR "rfb_encode.h: ${_rfb_n} exports matched; the"
                            " declaration regex above no longer fits the header")
    endif()

    function(rfb_rename_exports target prefix)
        foreach(_n IN LISTS RFB_EXPORTS)
            string(REGEX REPLACE "^rfb_" "${prefix}_" _r "${_n}")
            target_compile_definitions(${target} PRIVATE "${_n}=${_r}")
        endforeach()
    endfunction()

    add_library(rfb_encode_020 STATIC "${CMAKE_SOURCE_DIR}/src/rfb/rfb_encode.c")
    target_include_directories(rfb_encode_020 PRIVATE ${AMINETXDUO_CORE_INCLUDES})
    target_compile_options(rfb_encode_020 PRIVATE
        -m68020 -ffunction-sections -fno-optimize-sibling-calls -Wall -Wextra)
    rfb_rename_exports(rfb_encode_020 rfb020)

    # The tree builds at -Os and the encoder's inner loop is where that shows:
    # at -Os GCC fetches the source longword a second time to store it into the
    # shadow rather than reusing the register it is already in.  Two more
    # copies price -O2 against it, on both instruction sets, in the same run.
    add_library(rfb_encode_o2 STATIC "${CMAKE_SOURCE_DIR}/src/rfb/rfb_encode.c")
    target_include_directories(rfb_encode_o2 PRIVATE ${AMINETXDUO_CORE_INCLUDES})
    target_compile_options(rfb_encode_o2 PRIVATE
        -m68000 -O2 -ffunction-sections -fno-optimize-sibling-calls
        -Wall -Wextra)
    rfb_rename_exports(rfb_encode_o2 rfbo2)

    add_library(rfb_encode_o220 STATIC "${CMAKE_SOURCE_DIR}/src/rfb/rfb_encode.c")
    target_include_directories(rfb_encode_o220 PRIVATE ${AMINETXDUO_CORE_INCLUDES})
    target_compile_options(rfb_encode_o220 PRIVATE
        -m68020 -O2 -ffunction-sections -fno-optimize-sibling-calls
        -Wall -Wextra)
    rfb_rename_exports(rfb_encode_o220 rfb220)

    add_library(rfbkern STATIC rfbkern.c)
    target_compile_options(rfbkern PRIVATE -Wall -Wextra)

    add_library(rfbkern020 STATIC rfbkern.c)
    target_compile_options(rfbkern020 PRIVATE -m68020 -Wall -Wextra)
    target_compile_definitions(rfbkern020 PRIVATE RFBK_020)

    add_executable(rfbprof rfbprof.c)
    target_link_libraries(rfbprof PRIVATE
        aminetxduo_rfb rfb_encode_020 rfb_encode_o2 rfb_encode_o220
        rfbkern rfbkern020 aminetxduo_common)
    target_link_options(rfbprof PRIVATE
        "-Wl,-Map=$<TARGET_FILE_DIR:rfbprof>/rfbprof.map")
    set_target_properties(rfbprof PROPERTIES OUTPUT_NAME "rfbprof")

    if(AMIGA_NDK_INCLUDE)
        target_include_directories(rfbprof PRIVATE "${AMIGA_NDK_INCLUDE}")
    endif()

endif()


# ------------------------------------------- the interleaved layout check --
#
# The encoder reads the real bitplanes now, so it depends on being told the
# layout, and a wrong stride shears the picture rather than failing.  Every
# .pfs capture is plane-major, so the host round trip can only interleave one
# synthetically.  rfbil asks graphics.library for a real interleaved BitMap,
# encodes it, decodes it with a receiver of its own and compares every byte of
# the picture -- and prints what layout the Workbench screen on the machine
# actually has, which is the question of whether the live path has ever run
# interleaved at all.
#
#   cmake --build build/cm --parallel --target rfbil
#   AMINETXDUO_RUN_TAG=il ./tools/amiberry-run.sh -t 300 -m A1200 \
#       build/cm/tests/perf/rfbil

if(CMAKE_CROSSCOMPILING)

    add_executable(rfbil rfbil.c)
    target_link_libraries(rfbil PRIVATE aminetxduo_rfb aminetxduo_common)
    set_target_properties(rfbil PROPERTIES OUTPUT_NAME "rfbil")

    if(AMIGA_NDK_INCLUDE)
        target_include_directories(rfbil PRIVATE "${AMIGA_NDK_INCLUDE}")
    endif()

endif()


# ---------------------------------------------------------- the sampler ----
#
# -DAMINETXDUO_PROFILER=ON only.  See prof/CMakeLists.txt for what it is and
# how it is verified before anything it prints is believed.
add_subdirectory(prof)

# WbJitter: the CPU a Workbench-priority task waits for while the stack moves
# data (AmiTCP_NG #9 is a Workbench frozen for the length of a transfer).  A
# task at PRI asks for one tick with Delay(1) and reads the E-clock when it
# gets back; a gap far past 20 ms is time Workbench would not have repainted.
# No NetX Duo in it: the transfer it measures against is any other program.
#
#   cmake --build build/cm --parallel --target wbjitter
#   copy build/cm/tests/perf/WbJitter to RAM: on the machine, then
#   RAM:WbJitter SECS 30  (idle)  and again under iperf / a fetch.
if(CMAKE_CROSSCOMPILING)
    add_executable(wbjitter wbjitter.c)
    target_link_libraries(wbjitter PRIVATE aminetxduo_common)
    target_include_directories(wbjitter PRIVATE ${AMINETXDUO_CORE_INCLUDES})
    if(AMIGA_NDK_INCLUDE)
        target_include_directories(wbjitter PRIVATE "${AMIGA_NDK_INCLUDE}")
    endif()
    target_compile_options(wbjitter PRIVATE -Wall -Wextra)
    set_target_properties(wbjitter PROPERTIES OUTPUT_NAME "WbJitter")
endif()
