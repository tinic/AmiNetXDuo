# Which datagrams a UDP socket may receive.
#
# An AmigaOS executable that opens bsdsocket.library and calls through the LVO
# table, linked against none of our code. Its interface comes from
# tests/tcpdrill/tapdev.c, a SANA-II device made at run time, so a datagram can
# carry a source address of the test's choosing -- which is the only way to ask
# whether a connected socket checks who sent one.
#
#   tests/udpdrill/run-udpdrill.sh -A -b build/cm

if(NOT CMAKE_CROSSCOMPILING)
    return()
endif()

add_executable(udpdrill_test
    udpdrill.c
    "${CMAKE_SOURCE_DIR}/tests/tcpdrill/tapdev.c")

target_include_directories(udpdrill_test PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/tests/tcpdrill"
    "${CMAKE_SOURCE_DIR}/src/sana2")

if(AMIGA_NDK_INCLUDE)
    target_include_directories(udpdrill_test PRIVATE "${AMIGA_NDK_INCLUDE}")
endif()

target_compile_options(udpdrill_test PRIVATE -Wall -Wextra)
set_target_properties(udpdrill_test PROPERTIES OUTPUT_NAME "UdpDrill")

if(AMINETXDUO_BSDSOCKET_LINK)
    add_dependencies(udpdrill_test bsdsocket_library)
endif()

if(AMIGA_SIZE)
    add_custom_command(TARGET udpdrill_test POST_BUILD
        COMMAND "${AMIGA_SIZE}" "$<TARGET_FILE:udpdrill_test>"
        COMMENT "UdpDrill size")
endif()
