# The compiler runtime, checked on the machine that runs it.
#
# src/common/ami_udivdi3.c carries two forms of seven routines now -- the
# portable ones and the MULU.L/DIVU.L ones -- and nothing compared them until
# this.  It is a cross-only target because there is nothing to test on a host:
# there libgcc supplies the real helpers and ami_udivdi3.c compiles to nothing.
#
#   cmake --build build --parallel --target rt_test
#   AMINETXDUO_RUN_TAG=rt ./tools/amiberry-run.sh -t 300 -m A600 \
#       build/tests/common/rt_test
#
# SPDX-License-Identifier: MIT

if(CMAKE_CROSSCOMPILING)

    add_executable(rt_test rt_test.c)
    target_link_libraries(rt_test PRIVATE aminetxduo_m68k_rt)
    set_target_properties(rt_test PROPERTIES OUTPUT_NAME "rt_test")

    if(AMIGA_NDK_INCLUDE)
        target_include_directories(rt_test PRIVATE "${AMIGA_NDK_INCLUDE}")
    endif()

endif()
