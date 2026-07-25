# crypto68k on-Amiga tests.
#
#   cmake -S . -B build/cm -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-m68k-amigaos.cmake \
#         -DCMAKE_BUILD_TYPE=Release
#   cmake --build build/cm --parallel --target crypto68k_test crypto68k_bench
#
#   AMINETXDUO_RUN_TAG=rsa20 ./tools/fsuae-run.sh        -t 300 build/cm/tests/crypto68k/crypto68k_test
#   AMINETXDUO_RUN_TAG=rsa30 ./tools/fsuae-run.sh -c 68030 -t 300 build/cm/tests/crypto68k/crypto68k_test
#   AMINETXDUO_RUN_TAG=rsb20 ./tools/fsuae-run.sh        -t 600 build/cm/tests/crypto68k/crypto68k_bench
#   AMINETXDUO_RUN_TAG=rsb30 ./tools/fsuae-run.sh -c 68030 -t 600 build/cm/tests/crypto68k/crypto68k_bench
#
# Two executables, not one, because they have very different run times: the
# correctness pass fits the harness's usual timeout, while the benchmark has to
# run a REFERENCE RSA-2048 private operation without CRT, which alone is over
# two minutes on the floor target.  Splitting them means a correctness
# regression is caught in a short run.
#
# SPDX-License-Identifier: MIT

add_library(crypto68k_testsupport STATIC
    c68k_support.c
    c68k_timer.c)
target_include_directories(crypto68k_testsupport PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(crypto68k_testsupport PUBLIC crypto68k)

if(AMIGA_NDK_INCLUDE)
    target_include_directories(crypto68k_testsupport PUBLIC "${AMIGA_NDK_INCLUDE}")
endif()

add_executable(crypto68k_test  c68k_test.c)
add_executable(crypto68k_bench c68k_bench.c)

foreach(prog crypto68k_test crypto68k_bench)
    target_link_libraries(${prog} PRIVATE
        crypto68k_testsupport crypto68k aminetxduo_common)
    set_target_properties(${prog} PROPERTIES OUTPUT_NAME "${prog}")
    if(AMIGA_SIZE)
        add_custom_command(TARGET ${prog} POST_BUILD
            COMMAND "${AMIGA_SIZE}" "$<TARGET_FILE:${prog}>"
            COMMENT "${prog} size")
    endif()
endforeach()
