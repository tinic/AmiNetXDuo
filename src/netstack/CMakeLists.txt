# The stack singleton (include/aminetxduo/netstack.h).
#
# This is what ties the four other components together: it owns the ThreadX
# kernel, the packet pool, the NX_IP instance, the SANA-II interfaces, the DHCP
# and AutoIP clients and the DNS resolver.

set(NETSTACK_SOURCES
    netstack.c
    netstack_baton.c
    netstack_dns.c)

# The floor build never compiles this file, so IPv6 costs it nothing at all --
# not even a branch on a "is IPv6 on" flag.
if(AMINETXDUO_IPV6)
    list(APPEND NETSTACK_SOURCES netstack_ipv6.c)
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

target_compile_options(aminetxduo_netstack PRIVATE -Wall -Wextra)
