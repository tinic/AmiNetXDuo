# Guest-side helpers for the command tests.
#
# Nothing here is installed.  ClockSet moves the guest's clock so that
# run-sntp.sh can start from the machine a real Amiga user has, one whose
# battery died, rather than from FS-UAE's default of "the host's wall clock",
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

# IdxProbe calls the same four RFC 3493 vectors IfProbe's last phase calls, but
# flushes after every one, so a transcript that stops names the call that hung
# rather than the offset DOS's buffer happened to fill at.
add_executable(test_idxprobe idxprobe.c)
target_include_directories(test_idxprobe PRIVATE ${AMINETXDUO_CORE_INCLUDES})
if(AMIGA_NDK_INCLUDE)
    target_include_directories(test_idxprobe PRIVATE "${AMIGA_NDK_INCLUDE}")
endif()
target_compile_options(test_idxprobe PRIVATE -Wall -Wextra)
set_target_properties(test_idxprobe PROPERTIES OUTPUT_NAME "IdxProbe")

# RtProbe drives AddRouteTagList/DeleteRouteTagList/GetRouteInfo/FreeRouteInfo
# through the published LVOs.  It walks the returned table the way the routing
# socket convention requires, advance by rtm_msglen, read the sockaddrs by
# their bit order in rtm_addrs, rather than by indexing an array of
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
# the guard bytes past the end of each copy, and, the one that no build can
# check, that pcd_tcp_state is 4.4BSD's enumeration and not NetX Duo's,
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
# VOID, so its only channel is the replied message, which is why the dullest
# assertion in that file (the message came back on the port) is the one that
# matters: an ENOSYS stub for it is a hang, not a refusal.
add_executable(test_aamprobe aamprobe.c)
target_include_directories(test_aamprobe PRIVATE ${AMINETXDUO_CORE_INCLUDES})
if(AMIGA_NDK_INCLUDE)
    target_include_directories(test_aamprobe PRIVATE "${AMIGA_NDK_INCLUDE}")
endif()
target_compile_options(test_aamprobe PRIVATE -Wall -Wextra)
set_target_properties(test_aamprobe PROPERTIES OUTPUT_NAME "AamProbe")

# McastRace runs two Processes with a base each against bsd_mcast_table[16].
# The row is claimed inside the bsd_nx_enter() bracket and a claim made after
# it would let a second base take the same row; the assertion is the only thing
# the loser can see, which is its own IP_DROP_MEMBERSHIP refused on a group it
# holds.  The window is aimed at rather than waited for, see the file.
add_executable(test_mcastrace mcastrace.c)
target_include_directories(test_mcastrace PRIVATE ${AMINETXDUO_CORE_INCLUDES})
if(AMIGA_NDK_INCLUDE)
    target_include_directories(test_mcastrace PRIVATE "${AMIGA_NDK_INCLUDE}")
endif()
target_compile_options(test_mcastrace PRIVATE -Wall -Wextra)
set_target_properties(test_mcastrace PROPERTIES OUTPUT_NAME "McastRace")

# MonProbe drives AddNetMonitorHookTagList/RemoveNetMonitorHook and the two
# call-site hook types.  The hook register convention (Hook in A0, NULL in A2,
# message in A1) is not the pair a reader would guess, and a wrong guess hands
# the message in the wrong register, so the probe poisons A2 beforehand and
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
# time, a server added twice and removed once has to still be there, and
# inet_pton()'s strictness is only visible next to inet_addr()'s laxity, which
# is why the probe asks both the same strings.
add_executable(test_dnsprobe dnsprobe.c)
target_include_directories(test_dnsprobe PRIVATE ${AMINETXDUO_CORE_INCLUDES})
if(AMIGA_NDK_INCLUDE)
    target_include_directories(test_dnsprobe PRIVATE "${AMIGA_NDK_INCLUDE}")
endif()
target_compile_options(test_dnsprobe PRIVATE -Wall -Wextra)
set_target_properties(test_dnsprobe PROPERTIES OUTPUT_NAME "DnsProbe")

# TcpHandoff runs the whole socket-to-file-handle sequence, accept(),
# ReleaseCopyOfSocket(), Open("TCP:OBTAIN="), SystemTagList(), against two
# stock AmigaDOS commands.  Linked against nothing of ours: it reaches
# bsdsocket.library through its published LVOs, as a third-party program does.
add_executable(test_tcphandoff tcphandoff.c)
target_include_directories(test_tcphandoff PRIVATE ${AMINETXDUO_CORE_INCLUDES})
if(AMIGA_NDK_INCLUDE)
    target_include_directories(test_tcphandoff PRIVATE "${AMIGA_NDK_INCLUDE}")
endif()
target_compile_options(test_tcphandoff PRIVATE -Wall -Wextra)
set_target_properties(test_tcphandoff PROPERTIES OUTPUT_NAME "TcpHandoff")

# SrcProbe measures the send direction of bind(): a datagram bound to an
# address has to arrive with that address as its source, and a TCP connect()
# that would have to leave from another one has to be refused.  Neither is
# reachable from a host build, both read NX_IP's live interface table, and
# a send that merely returns len proves nothing, so the assertion is on the
# source address the receiver saw.
add_executable(test_srcprobe srcprobe.c)
target_include_directories(test_srcprobe PRIVATE ${AMINETXDUO_CORE_INCLUDES})
if(AMIGA_NDK_INCLUDE)
    target_include_directories(test_srcprobe PRIVATE "${AMIGA_NDK_INCLUDE}")
endif()
target_compile_options(test_srcprobe PRIVATE -Wall -Wextra)
set_target_properties(test_srcprobe PROPERTIES OUTPUT_NAME "SrcProbe")
# IfNames is the Developer drawer's own example, built here so the drawer
# cannot ship incomplete.  The drawer is staged into the build tree and that
# staged copy plus the NDK is the WHOLE include path, no
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

# Again at build time, so editing a drawer header or example is not silently
# compiled from a stale copy staged at the last configure.  It is a handful of
# cp's; the targets below order themselves after it.
add_custom_target(developer_drawer ALL
    COMMAND "${CMAKE_SOURCE_DIR}/tools/stage-developer.sh"
            "${AMINETXDUO_DEVELOPER_STAGE}"
    COMMENT "staging the Developer drawer"
    VERBATIM)

add_executable(test_ifnames "${AMINETXDUO_DEVELOPER_STAGE}/examples/IfNames.c")
target_include_directories(test_ifnames PRIVATE
                           "${AMINETXDUO_DEVELOPER_STAGE}/include")
if(AMIGA_NDK_INCLUDE)
    target_include_directories(test_ifnames PRIVATE "${AMIGA_NDK_INCLUDE}")
endif()
target_compile_options(test_ifnames PRIVATE -Wall -Wextra)
set_target_properties(test_ifnames PROPERTIES OUTPUT_NAME "IfNames")
add_dependencies(test_ifnames developer_drawer)

# V6Only is the other half of the same check, for the half of the drawer that
# has no vectors: IPPROTO_IPV6, IPV6_V6ONLY, INET6_ADDRSTRLEN,
# IN6ADDR_LOOPBACK_INIT, the IN6_IS_ADDR_* macros and sockaddr_storage, none
# of which the NDK defines.  Same include path as IfNames, the staged drawer
# and the NDK, nothing else.
add_executable(test_v6only "${AMINETXDUO_DEVELOPER_STAGE}/examples/V6Only.c")
target_include_directories(test_v6only PRIVATE
                           "${AMINETXDUO_DEVELOPER_STAGE}/include")
if(AMIGA_NDK_INCLUDE)
    target_include_directories(test_v6only PRIVATE "${AMIGA_NDK_INCLUDE}")
endif()
target_compile_options(test_v6only PRIVATE -Wall -Wextra)
set_target_properties(test_v6only PROPERTIES OUTPUT_NAME "V6Only")
add_dependencies(test_v6only developer_drawer)

# CycleDrill cycles the library and the interface rather than using them:
# OpenLibrary/CloseLibrary including the last-close expunge, Online/Offline
# through NETCTRL_INTERFACE_UP/DOWN, RemoveInterface/AddInterfaceTagList with
# sockets open over the top, and an expunge attempted while it still holds the
# library.  It reads NETSTATUS_HEALTH between cycles, which is where
# allocations outstanding, sockets alive and packet buffers free already live.
add_executable(test_cycledrill cycledrill.c)
target_include_directories(test_cycledrill PRIVATE ${AMINETXDUO_CORE_INCLUDES})
if(AMIGA_NDK_INCLUDE)
    target_include_directories(test_cycledrill PRIVATE "${AMIGA_NDK_INCLUDE}")
endif()
target_compile_options(test_cycledrill PRIVATE -Wall -Wextra)
set_target_properties(test_cycledrill PROPERTIES OUTPUT_NAME "CycleDrill")

# ApiDrill calls every one of the 150 LVOs in a loop and asserts the machine is
# where it started: AvailMem, NETSTATUS_HEALTH's allocations and sockets, and
# the three things that live on the CALLER and not in the library, tc_SigAlloc,
# SysBase->PortList and SysBase->SemaphoreList.  The commands reach about
# thirty entry points; the rest is what nothing has ever watched.  The table is
# checked for being dense and in LVO order, so a vector nobody wrote a row for
# shifts every later row and fails rather than going quietly uncovered.
add_executable(test_apidrill apidrill.c)
target_include_directories(test_apidrill PRIVATE ${AMINETXDUO_CORE_INCLUDES})
if(AMIGA_NDK_INCLUDE)
    target_include_directories(test_apidrill PRIVATE "${AMIGA_NDK_INCLUDE}")
endif()
target_compile_options(test_apidrill PRIVATE -Wall -Wextra)
set_target_properties(test_apidrill PROPERTIES OUTPUT_NAME "ApiDrill")

# ResolveBreak times a name lookup against a name server on a network nothing
# routes (RFC 5737 TEST-NET-1), with and without the break signal arriving in
# the middle.  A build cannot show either half and neither can a lookup against
# a server that works: the two numbers are how long the lookup blocks and how
# long after Ctrl-C it comes back.  Published LVOs only, so the same binary
# measures any bsdsocket.library, which is how a before and an after are
# comparable.
add_executable(test_trbreak trbreak.c)
target_include_directories(test_trbreak PRIVATE ${AMINETXDUO_CORE_INCLUDES})
if(AMIGA_NDK_INCLUDE)
    target_include_directories(test_trbreak PRIVATE "${AMIGA_NDK_INCLUDE}")
endif()
target_compile_options(test_trbreak PRIVATE -Wall -Wextra)
set_target_properties(test_trbreak PROPERTIES OUTPUT_NAME "TrBreak")

add_executable(test_resolvebreak resolvebreak.c)
target_include_directories(test_resolvebreak PRIVATE ${AMINETXDUO_CORE_INCLUDES})
if(AMIGA_NDK_INCLUDE)
    target_include_directories(test_resolvebreak PRIVATE "${AMIGA_NDK_INCLUDE}")
endif()
target_compile_options(test_resolvebreak PRIVATE -Wall -Wextra)
set_target_properties(test_resolvebreak PROPERTIES OUTPUT_NAME "ResolveBreak")

# EvPost asks whether a socket event can Signal() a Task that has exited.  It
# spawns a child that opens the library, binds a socket and dies without
# closing either, then AllocAbs()es the freed Task so the block it reads is one
# it owns, and sends a datagram.  A second child does the same correctly and
# stays alive: it is what tells a latched pointer apart from a stimulus that
# never arrived.  Published LVOs only, so the same binary measures a build with
# the fix and one without.
add_executable(test_evpost evpost.c)
target_include_directories(test_evpost PRIVATE ${AMINETXDUO_CORE_INCLUDES})
if(AMIGA_NDK_INCLUDE)
    target_include_directories(test_evpost PRIVATE "${AMIGA_NDK_INCLUDE}")
endif()
target_compile_options(test_evpost PRIVATE -Wall -Wextra)
set_target_properties(test_evpost PROPERTIES OUTPUT_NAME "EvPost")

# ShutProbe asks what a shutdown does to the programs using the network: two
# holder processes, one blocked inside the library in WaitSelect() and one
# waiting on its own signals outside it, then the command, then the grace
# period.  It reads bsdsocket.library's open count off the master base in
# SysBase->LibList rather than through a base of its own, because opening the
# library to look at it would restart a stack that had just gone down.
add_executable(test_shutprobe shutprobe.c)
target_include_directories(test_shutprobe PRIVATE ${AMINETXDUO_CORE_INCLUDES})
if(AMIGA_NDK_INCLUDE)
    target_include_directories(test_shutprobe PRIVATE "${AMIGA_NDK_INCLUDE}")
endif()
target_compile_options(test_shutprobe PRIVATE -Wall -Wextra)
set_target_properties(test_shutprobe PROPERTIES OUTPUT_NAME "ShutProbe")
