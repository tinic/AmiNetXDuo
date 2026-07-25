# The stack singleton (include/aminetxduo/netstack.h).
#
# This is what ties the four other components together: it owns the ThreadX
# kernel, the packet pool, the NX_IP instance, the SANA-II interfaces, the DHCP
# and AutoIP clients and the DNS resolver.

add_library(aminetxduo_netstack STATIC
    netstack.c
    netstack_baton.c
    netstack_dns.c)

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
