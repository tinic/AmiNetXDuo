# The PC sampler.  A tool, not a shipped feature: nothing here is built unless
# -DAMINETXDUO_PROFILER=ON asks for it, and nothing in src/ knows it exists.
#
#   cmake -S . -B build/p20 -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-m68k-amigaos.cmake \
#         -DCMAKE_BUILD_TYPE=Release -DAMINETXDUO_CPU=68020 -DAMINETXDUO_PROFILER=ON
#   cmake --build build/p20 --parallel --target profverify tcpprof
#
# Establish the sampler before believing it, on more than one CPU, because the
# exception frame is what is in question:
#
#   ./tools/fsuae-run.sh -x -t 600 -m A500  build/p00/tests/perf/prof/profverify
#   ./tools/fsuae-run.sh -x -t 600 -m A1200 build/p20/tests/perf/prof/profverify
#
# Then the transfer, and the ranking:
#
#   ./tools/fsuae-run.sh -x -t 1200 -m A1200 build/p20/tests/perf/prof/tcpprof
#   tools/prof-report.py build/testhd/profile-loop.bin \
#       --exe build/p20/tests/perf/prof/tcpprof \
#       --map build/p20/tests/perf/prof/tcpprof.map \
#       --objdir build/p20/tests/perf/prof --phase transfer
#
# SPDX-License-Identifier: MIT

if(NOT CMAKE_CROSSCOMPILING OR NOT AMINETXDUO_PROFILER)
    return()
endif()

# prof_vector.S needs no arch flags -- MOVEC is hand-encoded there precisely so
# this file assembles for the 68000 profile too -- but pass them anyway so the
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
    target_link_options(${name} PRIVATE
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

# The measurement.  Same fabric as perf_test.c's end-to-end cases.
add_executable(tcpprof tcpprof.c)
target_link_libraries(tcpprof PRIVATE
    aminetxduo_prof netxduo_port aminetxduo_common)
aminetxduo_prof_target(tcpprof)
