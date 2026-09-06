# The PC sampler.  A tool, not a shipped feature: nothing here is built unless
# -DAMINETXDUO_PROFILER=ON asks for it, and nothing in src/ knows it exists.
#
#   cmake -S . -B build/p20 -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-m68k-amigaos.cmake \
#         -DCMAKE_BUILD_TYPE=Release -DAMINETXDUO_CPU=68020 -DAMINETXDUO_PROFILER=ON \
#         -DAMINETXDUO_LTO=OFF
#
# -DAMINETXDUO_LTO=OFF IS NOT OPTIONAL.  With LTO the whole program lands in one
# /tmp/cc*.ltrans0.ltrans.o that the linker deletes, tcpprof.c.obj contributes
# 0 bytes marked "symbol from plugin", and prof-report.py's per-object nm pass
# -- its only route to the statics -- has nothing left to read.  Every sample
# then resolves to the nearest object parsed before it, and the report blames
# crt0.o for half the run.
#   cmake --build build/p20 --parallel --target profverify tcpprof
#
# Establish the sampler before believing it, on more than one CPU, because the
# exception frame is what is in question:
#
#   ./tools/amiberry-run.sh -t 600 -m A500  build/p00/tests/perf/prof/profverify
#   ./tools/amiberry-run.sh -t 600 -m A1200 build/p20/tests/perf/prof/profverify
#
# Then the transfer, and the ranking:
#
#   ./tools/amiberry-run.sh -t 1200 -m A1200 build/p20/tests/perf/prof/tcpprof
#   tools/prof-report.py build/amiberry-testhd-amiberry/profile-loop.bin \
#       --exe build/p20/tests/perf/prof/tcpprof \
#       --map build/p20/tests/perf/prof/tcpprof.map \
#       --objdir build/p20/tests/perf/prof --phase transfer
#
# SPDX-License-Identifier: MIT

if(NOT CMAKE_CROSSCOMPILING OR NOT AMINETXDUO_PROFILER)
    return()
endif()

# prof_vector.S needs no arch flags, MOVEC is hand-encoded there precisely so
# this file assembles for the 68000 profile too, but pass them anyway so the
# object matches the rest of the build.
add_library(aminetxduo_prof STATIC prof.c prof_vector.S)
target_include_directories(aminetxduo_prof PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(aminetxduo_prof PUBLIC aminetxduo_common)
set_source_files_properties(prof_vector.S PROPERTIES
    COMPILE_OPTIONS "${AMIGA_ARCH_FLAGS}")

if(AMIGA_NDK_INCLUDE)
    target_include_directories(aminetxduo_prof PUBLIC "${AMIGA_NDK_INCLUDE}")
endif()

# A linker map for every profiled binary.  tools/prof-report.py needs it: the
# statics that HUNK_SYMBOL drops are recoverable only from the map's placement
# of each object plus nm on that object.
function(aminetxduo_prof_target name)
    set_target_properties(${name} PROPERTIES OUTPUT_NAME "${name}")
    # --gc-sections for correctness, not size: without it these link at 12
    # hunks and 0 HUNK_RELOC32, so LoadSeg relocates nothing.  Same defect and
    # same fix as tools/profiler/CMakeLists.txt:44, which explains it.
    target_link_options(${name} PRIVATE
        -Wl,--gc-sections
        "-Wl,-Map=$<TARGET_FILE_DIR:${name}>/${name}.map")
    if(AMIGA_NDK_INCLUDE)
        target_include_directories(${name} PRIVATE "${AMIGA_NDK_INCLUDE}")
    endif()
    if(AMIGA_SIZE)
        add_custom_command(TARGET ${name} POST_BUILD
            COMMAND "${AMIGA_SIZE}" "$<TARGET_FILE:${name}>"
            COMMENT "${name} size")
    endif()
endfunction()

# The self-test.  Kernels with known byte ranges, so "did the sampler report
# the PC it thinks it did" has an answer that is not a matter of opinion.
add_executable(profverify profverify.c profverify.S)
target_link_libraries(profverify PRIVATE aminetxduo_prof aminetxduo_common)
set_source_files_properties(profverify.S PROPERTIES
    COMPILE_OPTIONS "${AMIGA_ARCH_FLAGS}")
aminetxduo_prof_target(profverify)

# The same sampler over a REAL transfer: the shipping bsdsocket.library, the
# SANA-II reader and the card the guest was booted with.  tcpprof below cannot
# answer anything about that path -- it runs both its phases over
# _nx_ram_network_driver -- and the step budget measures it only as legs, which
# say how long and never which function.
# The iperf core is compiled in, not linked: src/tools builds each command
# with its own SOURCES list (aminetxduo_add_tool) and publishes no library,
# and the point of reusing those files is that what this measures is the same
# code tests/tools/run-iperf.sh measures.
add_executable(wireprof wireprof.c
    "${CMAKE_SOURCE_DIR}/src/tools/iperfcore.c"
    "${CMAKE_SOURCE_DIR}/src/tools/iperfwire.c"
    "${CMAKE_SOURCE_DIR}/src/tools/toolsock.c"
    "${CMAKE_SOURCE_DIR}/src/tools/toolbudget.c"
    "${CMAKE_SOURCE_DIR}/src/tools/tool_util.c"
    "${CMAKE_SOURCE_DIR}/src/tools/tool_diag.c"
    "${CMAKE_SOURCE_DIR}/src/tools/netstack_weak.c"
    # Deliberately outside aminetxduo_config (src/config/CMakeLists.txt:10):
    # it carries the words for the advice codes, so only a consumer that can
    # print them links it, and tool_diag.c is one.
    "${CMAKE_SOURCE_DIR}/src/config/config_advice.c")
# The same set aminetxduo_add_tool() gives every command: tool_diag.c reaches
# sana2_device.h, and TOOLS_COMMON_SOURCES is not optional -- toolsock.c calls
# into it.
target_include_directories(wireprof PRIVATE
    ${AMINETXDUO_CORE_INCLUDES}
    "${CMAKE_SOURCE_DIR}/src/tools"
    "${CMAKE_SOURCE_DIR}/src/sana2")
target_link_libraries(wireprof PRIVATE
    aminetxduo_prof aminetxduo_config aminetxduo_common)
aminetxduo_prof_target(wireprof)

# The measurement.  Same fabric as perf_test.c's end-to-end cases.
add_executable(tcpprof tcpprof.c)
target_link_libraries(tcpprof PRIVATE
    aminetxduo_prof netxduo_port aminetxduo_common)
aminetxduo_prof_target(tcpprof)
