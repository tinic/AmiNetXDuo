# TCP end to end over a simulated link: two whole NetX Duo stacks, a wire
# with a rate, a delay, a tail-drop queue and seeded loss, and ThreadX on a
# virtual clock.  host/test_netsim_tcp_host.c says what each scenario
# asserts and why; host/netsim.h what the wire models.
#
# All of NetX Duo's and ThreadX's common sources, with the shipping nx_user.h
# and every TCP option the tree sets (directory-scoped, so inherited here).
# Two vendored files the cross build replaces stay in: the m68k checksum
# (src/net68k) and the IPv6 source selection (src/ipv6) are not host code,
# and neither is on the path under test.
#
# SPDX-License-Identifier: MIT

if(CMAKE_CROSSCOMPILING)
    return()
endif()

# Another NetX Duo tree to run the same scenarios against, for bisecting a
# regression the scenarios caught: -DAMINETXDUO_NETSIM_NETXDUO=<checkout>.
set(AMINETXDUO_NETSIM_NETXDUO "${AMINETXDUO_NETXDUO}" CACHE PATH
    "NetX Duo tree the netsim test builds (default: the submodule)")
set(_netsim_nx "${AMINETXDUO_NETSIM_NETXDUO}")

# PRR's state goes on the trace's acknowledgment lines where the tree has it.
file(STRINGS "${_netsim_nx}/common/inc/nx_api.h" _netsim_prr
     REGEX "nx_tcp_socket_prr_out;")

file(GLOB _netsim_threadx "${AMINETXDUO_THREADX}/common/src/*.c")
file(GLOB _netsim_netxduo "${_netsim_nx}/common/src/*.c")

if(NOT AMINETXDUO_IPV6)
    list(FILTER _netsim_netxduo EXCLUDE REGEX "/(nx_icmpv6|nx_ipv6|nx_nd|nx_mld)_")
endif()

add_executable(test_netsim_tcp
    host/test_netsim_tcp_host.c
    host/netsim_link.c
    host/tx_sim_port.c
    "${CMAKE_SOURCE_DIR}/src/bsdsocket/bsdsocket_window.c"
    ${_netsim_threadx}
    ${_netsim_netxduo})

target_include_directories(test_netsim_tcp PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/host/shim"         # tx_port.h, the sim's
    "${CMAKE_CURRENT_SOURCE_DIR}/host"
    "${_netsim_nx}/ports/linux/gnu/inc"             # nx_port.h
    "${CMAKE_SOURCE_DIR}/port/netxduo-amiga/inc"    # nx_user.h, ours
    "${_netsim_nx}/common/inc"
    "${AMINETXDUO_THREADX}/common/inc"
    "${CMAKE_SOURCE_DIR}/src"                       # thread_priorities.h
    "${CMAKE_SOURCE_DIR}/src/bsdsocket"             # bsdsocket_window.h
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/src/config/test/shim")     # exec/types.h

# Error checking off, as the shipped library is built.
target_compile_definitions(test_netsim_tcp PRIVATE
    NX_INCLUDE_USER_DEFINE_FILE
    NX_DISABLE_ERROR_CHECKING
    TX_DISABLE_ERROR_CHECKING)

if(_netsim_prr)
    target_compile_definitions(test_netsim_tcp PRIVATE NETSIM_HAVE_PRR)
endif()

find_package(Threads REQUIRED)
target_link_libraries(test_netsim_tcp PRIVATE Threads::Threads)

set_target_properties(test_netsim_tcp PROPERTIES C_STANDARD 11)

foreach(_scenario clean bottleneck loss reverse shallow)
    add_test(NAME netsim_tcp_${_scenario} COMMAND test_netsim_tcp ${_scenario})
endforeach()
