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

# --------------------------------------------------------------- host tier --
#
# The same seven routines against the host's own arithmetic.  Nothing in
# ami_udivdi3.c is AmigaOS: it is 64-bit division written for a part that has
# no 64-bit divide instruction, and the arms a 68000 and a 68060 take are
# plain C.  Compiled here with its assembly arms off, that is a pure function
# of two integers, checked against a machine that does have the instruction.
#
# The symbols are renamed so the host's libgcc keeps its own definitions; the
# test declares the renamed ones.  rt_test above stays the only check of the
# DIVU.L and MULU.L arms, which no host can compile.

if(NOT CMAKE_CROSSCOMPILING)

    add_executable(test_rt_host
        host/test_rt_host.c
        "${CMAKE_SOURCE_DIR}/src/common/ami_udivdi3.c")

    target_compile_definitions(test_rt_host PRIVATE
        __udivmoddi4=anxd_udivmoddi4
        __udivdi3=anxd_udivdi3
        __umoddi3=anxd_umoddi3
        __divdi3=anxd_divdi3
        __moddi3=anxd_moddi3
        __muldi3=anxd_muldi3
        __mulsi3=anxd_mulsi3
        __udivsi3=anxd_udivsi3
        __umodsi3=anxd_umodsi3
        __divsi3=anxd_divsi3
        __modsi3=anxd_modsi3
        __lshrdi3=anxd_lshrdi3
        __ashldi3=anxd_ashldi3
        __ashrdi3=anxd_ashrdi3)

    include(CTest)
    add_test(NAME rt_host COMMAND test_rt_host)

endif()
