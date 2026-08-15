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

# perf_test defines _nx_ip_checksum_compute() itself, so it can only be built
# when the vendored object is out of the core, i.e. when the replacement is
# switched on.  With -DAMINETXDUO_NET68K_CHECKSUM=OFF the two definitions would
# collide, and the A/B it exists to run would be meaningless anyway.
if(CMAKE_CROSSCOMPILING AND AMINETXDUO_NET68K_CHECKSUM)

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

    target_compile_definitions(test_net68k_rxverify PRIVATE
        AMINETXDUO_RX_VERIFY
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
# source at -m68020 with its eight public symbols renamed, so one run times
# both codegens over the same bytes in the same second.  That is the only way
# to answer "what is the -m68000 codegen costing the hottest loop in the
# program" without comparing two runs on a shared host.

if(CMAKE_CROSSCOMPILING)

    add_library(rfb_encode_020 STATIC "${CMAKE_SOURCE_DIR}/src/rfb/rfb_encode.c")
    target_include_directories(rfb_encode_020 PRIVATE ${AMINETXDUO_CORE_INCLUDES})
    target_compile_options(rfb_encode_020 PRIVATE
        -m68020 -ffunction-sections -fno-optimize-sibling-calls -Wall -Wextra)
    target_compile_definitions(rfb_encode_020 PRIVATE
        rfb_packbits=rfb020_packbits
        rfb_unpackbits=rfb020_unpackbits
        rfb_shadow_size=rfb020_shadow_size
        rfb_scratch_size=rfb020_scratch_size
        rfb_worst_case_frame=rfb020_worst_case_frame
        rfb_scroll_defaults=rfb020_scroll_defaults
        rfb_encoder_init=rfb020_encoder_init
        rfb_encode_frame=rfb020_encode_frame)

    # The tree builds at -Os and the encoder's inner loop is where that shows:
    # at -Os GCC fetches the source longword a second time to store it into the
    # shadow rather than reusing the register it is already in.  Two more
    # copies price -O2 against it, on both instruction sets, in the same run.
    add_library(rfb_encode_o2 STATIC "${CMAKE_SOURCE_DIR}/src/rfb/rfb_encode.c")
    target_include_directories(rfb_encode_o2 PRIVATE ${AMINETXDUO_CORE_INCLUDES})
    target_compile_options(rfb_encode_o2 PRIVATE
        -m68000 -O2 -ffunction-sections -fno-optimize-sibling-calls
        -Wall -Wextra)
    target_compile_definitions(rfb_encode_o2 PRIVATE
        rfb_packbits=rfbo2_packbits
        rfb_unpackbits=rfbo2_unpackbits
        rfb_shadow_size=rfbo2_shadow_size
        rfb_scratch_size=rfbo2_scratch_size
        rfb_worst_case_frame=rfbo2_worst_case_frame
        rfb_scroll_defaults=rfbo2_scroll_defaults
        rfb_encoder_init=rfbo2_encoder_init
        rfb_encode_frame=rfbo2_encode_frame)

    add_library(rfb_encode_o220 STATIC "${CMAKE_SOURCE_DIR}/src/rfb/rfb_encode.c")
    target_include_directories(rfb_encode_o220 PRIVATE ${AMINETXDUO_CORE_INCLUDES})
    target_compile_options(rfb_encode_o220 PRIVATE
        -m68020 -O2 -ffunction-sections -fno-optimize-sibling-calls
        -Wall -Wextra)
    target_compile_definitions(rfb_encode_o220 PRIVATE
        rfb_packbits=rfb220_packbits
        rfb_unpackbits=rfb220_unpackbits
        rfb_shadow_size=rfb220_shadow_size
        rfb_scratch_size=rfb220_scratch_size
        rfb_worst_case_frame=rfb220_worst_case_frame
        rfb_scroll_defaults=rfb220_scroll_defaults
        rfb_encoder_init=rfb220_encoder_init
        rfb_encode_frame=rfb220_encode_frame)

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


# ---------------------------------------------------------- the sampler ----
#
# -DAMINETXDUO_PROFILER=ON only.  See prof/CMakeLists.txt for what it is and
# how it is verified before anything it prints is believed.
add_subdirectory(prof)
