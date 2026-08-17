# The socket option surface: setsockopt, getsockopt and IoctlSocket.
#
# Two halves, because the findings are of two kinds.
#
#   host/  the option NUMBERS, checked against the BSD/KAME numbering the
#          header set follows.  A fact about integers, so it runs on every
#          build rather than only where an emulator does.
#
#   the guest test, which is what an application sees: an AmigaOS executable
#   that opens bsdsocket.library and calls through the LVO table, linked
#   against nothing of ours.  Its interface comes from tests/tcpdrill/tapdev.c,
#   a SANA-II device made at run time, so it needs no a2065.device.
#
#   AMINETXDUO_RUN_TAG=sockopt ./tests/sockopt/run-sockopt.sh -b build/sockopt

if(NOT CMAKE_CROSSCOMPILING)

    add_executable(test_sockopt_numbers host/test_optnum_host.c)

    target_compile_definitions(test_sockopt_numbers PRIVATE
        AMINETXDUO_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

    include(CTest)
    add_test(NAME sockopt_numbers COMMAND test_sockopt_numbers)

    return()
endif()

add_executable(sockopt_test
    sockopt_test.c
    "${CMAKE_SOURCE_DIR}/tests/tcpdrill/tapdev.c")

target_include_directories(sockopt_test PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/tests/tcpdrill"
    "${CMAKE_SOURCE_DIR}/src/sana2")

if(AMIGA_NDK_INCLUDE)
    target_include_directories(sockopt_test PRIVATE "${AMIGA_NDK_INCLUDE}")
endif()

# The multicast options only exist in a multicast build, and the test skips
# them without this.
if(AMINETXDUO_MULTICAST)
    target_compile_definitions(sockopt_test PRIVATE AMINETXDUO_MULTICAST)
endif()

set_target_properties(sockopt_test PROPERTIES OUTPUT_NAME "sockopt_test")

if(AMINETXDUO_BSDSOCKET_LINK)
    add_dependencies(sockopt_test bsdsocket_library)
endif()
