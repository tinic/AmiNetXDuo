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

# IfProbe drives ObtainInterfaceList/QueryInterfaceTagList/ReleaseInterfaceList
# through the published LVOs, as Roadie and NetMon do.  It links against
# nothing of ours: the list node layout and the IFQ_* tag conventions are the
# whole point, and a probe that shared a header with the implementation could
# not catch either of them being wrong.
add_executable(test_ifprobe ifprobe.c)
target_include_directories(test_ifprobe PRIVATE ${AMINETXDUO_CORE_INCLUDES})
if(AMIGA_NDK_INCLUDE)
    target_include_directories(test_ifprobe PRIVATE "${AMIGA_NDK_INCLUDE}")
endif()
target_compile_options(test_ifprobe PRIVATE -Wall -Wextra)
set_target_properties(test_ifprobe PROPERTIES OUTPUT_NAME "IfProbe")

# RtProbe drives AddRouteTagList/DeleteRouteTagList/GetRouteInfo/FreeRouteInfo
# through the published LVOs.  It walks the returned table the way the routing
# socket convention requires -- advance by rtm_msglen, read the sockaddrs by
# their bit order in rtm_addrs -- rather than by indexing an array of
# rt_msghdr, which is what the prototype alone would suggest and which would
# walk off the end of the first entry.
add_executable(test_rtprobe rtprobe.c)
target_include_directories(test_rtprobe PRIVATE ${AMINETXDUO_CORE_INCLUDES})
if(AMIGA_NDK_INCLUDE)
    target_include_directories(test_rtprobe PRIVATE "${AMIGA_NDK_INCLUDE}")
endif()
target_compile_options(test_rtprobe PRIVATE -Wall -Wextra)
set_target_properties(test_rtprobe PROPERTIES OUTPUT_NAME "RtProbe")

# StatProbe drives GetNetworkStatistics().  It asserts the byte-count return,
# the guard bytes past the end of each copy, and -- the one that no build can
# check -- that pcd_tcp_state is 4.4BSD's enumeration and not NetX Duo's,
# which diverge after CLOSE_WAIT.
add_executable(test_statprobe statprobe.c)
target_include_directories(test_statprobe PRIVATE ${AMINETXDUO_CORE_INCLUDES})
if(AMIGA_NDK_INCLUDE)
    target_include_directories(test_statprobe PRIVATE "${AMIGA_NDK_INCLUDE}")
endif()
target_compile_options(test_statprobe PRIVATE -Wall -Wextra)
set_target_properties(test_statprobe PROPERTIES OUTPUT_NAME "StatProbe")

# AamProbe drives CreateAddrAllocMessageA/DeleteAddrAllocMessage/
# BeginInterfaceConfig/AbortInterfaceConfig.  BeginInterfaceConfig returns
# VOID, so its only channel is the replied message -- which is why the dullest
# assertion in that file (the message came back on the port) is the one that
# matters: an ENOSYS stub for it is a hang, not a refusal.
add_executable(test_aamprobe aamprobe.c)
target_include_directories(test_aamprobe PRIVATE ${AMINETXDUO_CORE_INCLUDES})
if(AMIGA_NDK_INCLUDE)
    target_include_directories(test_aamprobe PRIVATE "${AMIGA_NDK_INCLUDE}")
endif()
target_compile_options(test_aamprobe PRIVATE -Wall -Wextra)
set_target_properties(test_aamprobe PROPERTIES OUTPUT_NAME "AamProbe")

# MonProbe drives AddNetMonitorHookTagList/RemoveNetMonitorHook and the two
# call-site hook types.  The hook register convention (Hook in A0, NULL in A2,
# message in A1) is not the pair a reader would guess, and a wrong guess hands
# the message in the wrong register -- so the probe poisons A2 beforehand and
# asserts it came back NULL.
add_executable(test_monprobe monprobe.c)
target_include_directories(test_monprobe PRIVATE ${AMINETXDUO_CORE_INCLUDES})
if(AMIGA_NDK_INCLUDE)
    target_include_directories(test_monprobe PRIVATE "${AMIGA_NDK_INCLUDE}")
endif()
target_compile_options(test_monprobe PRIVATE -Wall -Wextra)
set_target_properties(test_monprobe PROPERTIES OUTPUT_NAME "MonProbe")

# DnsProbe drives AddDomainNameServer/RemoveDomainNameServer/
# ObtainDomainNameServerList, Get/SetDefaultDomainName and inet_pton against a
# running stack.  The nesting rule and dnsn_UseCount's sign only exist at run
# time -- a server added twice and removed once has to still be there -- and
# inet_pton()'s strictness is only visible next to inet_addr()'s laxity, which
# is why the probe asks both the same strings.
add_executable(test_dnsprobe dnsprobe.c)
target_include_directories(test_dnsprobe PRIVATE ${AMINETXDUO_CORE_INCLUDES})
if(AMIGA_NDK_INCLUDE)
    target_include_directories(test_dnsprobe PRIVATE "${AMIGA_NDK_INCLUDE}")
endif()
target_compile_options(test_dnsprobe PRIVATE -Wall -Wextra)
set_target_properties(test_dnsprobe PROPERTIES OUTPUT_NAME "DnsProbe")

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

# IfNames is the Developer drawer's own example, built here so the drawer
# cannot ship incomplete.  The drawer is staged into the build tree and that
# staged copy plus the NDK is the WHOLE include path -- no
# AMINETXDUO_CORE_INCLUDES, so a type or a prototype that only exists in
# include/aminetxduo/ and never reaches Developer/ fails to compile.
#
# It replaces ifprobe.c's hand-written `jsr a6@(-882:W)` for the four RFC 3493
# vectors; ifprobe keeps its copy, and the two agreeing is the check that the
# drawer's LVOs are the ones the library really uses.
set(AMINETXDUO_DEVELOPER_STAGE "${CMAKE_BINARY_DIR}/Developer")
execute_process(
    COMMAND "${CMAKE_SOURCE_DIR}/tools/stage-developer.sh"
            "${AMINETXDUO_DEVELOPER_STAGE}"
    RESULT_VARIABLE _dev_stage_rc)
if(NOT _dev_stage_rc EQUAL 0)
    message(FATAL_ERROR "tools/stage-developer.sh failed (${_dev_stage_rc})")
endif()

add_executable(test_ifnames "${AMINETXDUO_DEVELOPER_STAGE}/examples/IfNames.c")
target_include_directories(test_ifnames PRIVATE
                           "${AMINETXDUO_DEVELOPER_STAGE}/include")
if(AMIGA_NDK_INCLUDE)
    target_include_directories(test_ifnames PRIVATE "${AMIGA_NDK_INCLUDE}")
endif()
target_compile_options(test_ifnames PRIVATE -Wall -Wextra)
set_target_properties(test_ifnames PROPERTIES OUTPUT_NAME "IfNames")
