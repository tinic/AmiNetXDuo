# The refused-connect leak of docs/RESEARCH.md 37.5 / 39, reduced.
#
# Produces an AmigaOS executable; it is not runnable on the build host and it
# needs the library it is testing staged next to it:
#
#   tests/leak/run-leak.sh
#
# Links nothing of the stack: it reaches bsdsocket.library through its LVOs,
# because a program that linked src/netstack would get a SECOND set of NetX
# Duo globals and measure the wrong stack (include/aminetxduo/netstatus.h).

if(NOT CMAKE_CROSSCOMPILING)
    return()
endif()

add_executable(refused_leak_test refused_leak_test.c)

target_include_directories(refused_leak_test PRIVATE
    "${CMAKE_SOURCE_DIR}/include")

if(AMIGA_NDK_INCLUDE)
    target_include_directories(refused_leak_test PRIVATE "${AMIGA_NDK_INCLUDE}")
endif()

set_target_properties(refused_leak_test PROPERTIES
    OUTPUT_NAME "refused_leak_test")
