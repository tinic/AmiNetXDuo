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

# Arm G used to be off because it wedged the calling task (docs/RESEARCH.md
# 41.4), and an arm that hangs means the six that pass never report.  The wedge
# was this file's own LVO stubs not telling GCC that a library call clobbers
# d1/a0/a1, so IoctlSocket(FIONBIO) got a stale request code and the dial
# became a blocking connect() that never returns, 42.  Fixed, so the arm is
# built by default and covers 37.4's accept path again.
#
# LEAK_BURST_DELAY=OFF removes the Delay(1) between the three dials, which is
# the back-to-back shape the wedge was first seen in and the one to run when
# touching the accept path.
option(LEAK_BURST_ARM "Build arm G of the leak test (three dials, then three accepts)" ON)
option(LEAK_BURST_DELAY "Keep the Delay(1) between arm G's dials" ON)

if(LEAK_BURST_ARM)
    target_compile_definitions(refused_leak_test PRIVATE LEAK_BURST_ARM=1)
endif()
if(NOT LEAK_BURST_DELAY)
    target_compile_definitions(refused_leak_test PRIVATE LEAK_BURST_DELAY=0)
endif()
