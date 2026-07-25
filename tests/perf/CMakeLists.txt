# The TCP data-path cost census.
#
#   cmake -S . -B build/cm -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-m68k-amigaos.cmake \
#         -DCMAKE_BUILD_TYPE=Release
#   cmake --build build/cm --parallel --target perf_test
#
#   AMINETXDUO_RUN_TAG=perf20 ./tools/fsuae-run.sh -t 900 build/cm/tests/perf/perf_test
#
# To price the C library's memcpy rather than net68k's, configure a second tree
# with -DAMINETXDUO_NET68K_MEMCPY=OFF: with the hook on, memcpy() resolves to
# n68k_copy_bytes() and the "memcpy" rows measure it under a second name.
#
# Only the 68020 run is meaningful.  FS-UAE's 68030 model does not charge for
# everything the 68020 model does (docs/RESEARCH.md, the P-256 result records
# the same caveat), so run it there as a correctness check and do not quote the
# numbers.
#
# SPDX-License-Identifier: MIT

# perf_test defines _nx_ip_checksum_compute() itself, so it can only be built
# when the vendored object is out of the core -- i.e. when the replacement is
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

# aminetxduo_sana2 is here for exactly one object -- sana2_copy.c, whose
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

    include(CTest)
    add_test(NAME net68k_checksum COMMAND test_net68k_checksum)

endif()
