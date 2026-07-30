# The stack singleton (include/aminetxduo/netstack.h).
#
# This is what ties the four other components together: it owns the ThreadX
# kernel, the packet pool, the NX_IP instance, the SANA-II interfaces, the DHCP
# and AutoIP clients and the DNS resolver.

set(NETSTACK_SOURCES
    netstack.c
    netstack_baton.c
    netstack_dns.c
    netstack_rexx.c)

# The floor build never compiles this file, so IPv6 costs it nothing at all --
# not even a branch on a "is IPv6 on" flag.
if(AMINETXDUO_IPV6)
    list(APPEND NETSTACK_SOURCES netstack_ipv6.c)
endif()

# Capture: registers the SANA-II interfaces with src/bpf/ and installs the
# NetX Duo IP filter that lo0 needs.  Nothing else in the tree calls src/bpf/,
# so without this the whole subsystem is unreachable code.
if(AMINETXDUO_BPF)
    list(APPEND NETSTACK_SOURCES netstack_capture.c)
endif()

# mDNS: the RFC 6762 responder and the ".local" branch in netstack_resolve().
# Same arrangement as IPv6 above -- a build without it does not compile the
# file, so the floor build pays nothing, not even a test on a flag.
if(AMINETXDUO_MDNS)
    list(APPEND NETSTACK_SOURCES netstack_mdns.c)
endif()

add_library(aminetxduo_netstack STATIC ${NETSTACK_SOURCES})

target_include_directories(aminetxduo_netstack
    PUBLIC  ${AMINETXDUO_CORE_INCLUDES}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(aminetxduo_netstack PUBLIC
    aminetxduo_common
    aminetxduo_config
    aminetxduo_sana2
    netxduo_addons
    threadx_port)

if(AMINETXDUO_BPF)
    target_link_libraries(aminetxduo_netstack PUBLIC aminetxduo_bpf)
endif()

target_compile_options(aminetxduo_netstack PRIVATE -Wall -Wextra)
