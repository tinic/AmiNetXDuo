# The API called from the stack a Shell command actually gets.
#
# Produces an AmigaOS executable; it needs the library it is testing staged
# next to it, like the other on-Amiga harnesses.
#
# Links nothing of the stack: it reaches bsdsocket.library through its LVOs,
# because a program that linked src/netstack would get a SECOND set of NetX Duo
# globals and measure the wrong stack (include/aminetxduo/netstatus.h).

if(NOT CMAKE_CROSSCOMPILING)
    return()
endif()

add_executable(stack_test stack_test.c)

target_include_directories(stack_test PRIVATE
    "${CMAKE_SOURCE_DIR}/include")

if(AMIGA_NDK_INCLUDE)
    target_include_directories(stack_test PRIVATE "${AMIGA_NDK_INCLUDE}")
endif()

set_target_properties(stack_test PROPERTIES OUTPUT_NAME "stack_test")

# 4096 is the AmigaDOS Shell default and the number this test is about. Raise
# it to find where the headroom actually starts rather than only to pass.
set(STACK_TEST_STACK "4096" CACHE STRING
    "Worker stack in bytes; 4096 is what the Shell hands out")
set(STACK_TEST_ROUNDS "64" CACHE STRING
    "Loopback connect/send/WaitSelect/recv rounds")

target_compile_definitions(stack_test PRIVATE
    ST_STACK=${STACK_TEST_STACK}UL
    ST_ROUNDS=${STACK_TEST_ROUNDS}UL)
