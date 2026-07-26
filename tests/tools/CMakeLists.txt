# Guest-side helpers for the command tests.
#
# Nothing here is installed.  ClockSet moves the guest's clock so that
# run-sntp.sh can start from the machine a real Amiga user has -- one whose
# battery died -- rather than from FS-UAE's default of "the host's wall clock",
# which is the one state in which the interesting half of the test cannot run.

add_executable(test_clockset clockset.c)
target_include_directories(test_clockset PRIVATE ${AMINETXDUO_CORE_INCLUDES})
if(AMIGA_NDK_INCLUDE)
    target_include_directories(test_clockset PRIVATE "${AMIGA_NDK_INCLUDE}")
endif()
target_compile_options(test_clockset PRIVATE -Wall -Wextra)
set_target_properties(test_clockset PROPERTIES OUTPUT_NAME "ClockSet")

# RouteProbe drives NETCTRL_ROUTE_ADD/DELETE and NETSTATUS_ROUTES directly,
# through the published LVOs, and then sends one datagram that only a static
# route can place.  tests/tools/run-routes.sh reads the answer out of the
# emulator's own frame log rather than out of anything this stack wrote.
add_executable(test_routeprobe routeprobe.c)
target_include_directories(test_routeprobe PRIVATE ${AMINETXDUO_CORE_INCLUDES})
if(AMIGA_NDK_INCLUDE)
    target_include_directories(test_routeprobe PRIVATE "${AMIGA_NDK_INCLUDE}")
endif()
target_compile_options(test_routeprobe PRIVATE -Wall -Wextra)
set_target_properties(test_routeprobe PROPERTIES OUTPUT_NAME "RouteProbe")

# TcpHandoff runs the whole socket-to-file-handle sequence -- accept(),
# ReleaseCopyOfSocket(), Open("TCP:OBTAIN="), SystemTagList() -- against two
# stock AmigaDOS commands.  Linked against nothing of ours: it reaches
# bsdsocket.library through its published LVOs, as a third-party program does.
add_executable(test_tcphandoff tcphandoff.c)
target_include_directories(test_tcphandoff PRIVATE ${AMINETXDUO_CORE_INCLUDES})
if(AMIGA_NDK_INCLUDE)
    target_include_directories(test_tcphandoff PRIVATE "${AMIGA_NDK_INCLUDE}")
endif()
target_compile_options(test_tcphandoff PRIVATE -Wall -Wextra)
set_target_properties(test_tcphandoff PROPERTIES OUTPUT_NAME "TcpHandoff")
