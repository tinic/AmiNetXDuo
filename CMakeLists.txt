cmake_minimum_required(VERSION 3.20)
project(AmiNetXDuo C ASM)

# ---------------------------------------------------------------- options ---

option(AMINETXDUO_IPV6 "Build the IPv6 dual stack"                    OFF)
option(AMINETXDUO_TLS  "Build the nx_secure TLS library (68030+)"     OFF)
option(AMINETXDUO_BPF  "Build the bpf_* raw path and filter VM"       ON)
option(AMINETXDUO_TESTS "Build the on-Amiga test harnesses"           ON)

set(AMINETXDUO_THREADX  "${CMAKE_CURRENT_SOURCE_DIR}/third_party/threadx")
set(AMINETXDUO_NETXDUO  "${CMAKE_CURRENT_SOURCE_DIR}/third_party/netxduo")

if(NOT EXISTS "${AMINETXDUO_THREADX}/common/inc/tx_api.h")
    message(FATAL_ERROR "Submodules missing. Run: git submodule update --init --recursive")
endif()

# ------------------------------------------------------------ include sets --

# The port headers (tx_port.h, nx_port.h, nx_user.h) must come FIRST on the
# include path -- ThreadX and NetX Duo pick their port up by plain #include.
set(AMINETXDUO_PORT_INCLUDES
    "${CMAKE_CURRENT_SOURCE_DIR}/port/threadx-amiga/inc"
    "${CMAKE_CURRENT_SOURCE_DIR}/port/netxduo-amiga/inc")

set(AMINETXDUO_CORE_INCLUDES
    ${AMINETXDUO_PORT_INCLUDES}
    "${AMINETXDUO_THREADX}/common/inc"
    "${AMINETXDUO_NETXDUO}/common/inc"
    "${CMAKE_CURRENT_SOURCE_DIR}/include")

if(AMIGA_NDK_INCLUDE)
    list(APPEND AMINETXDUO_CORE_INCLUDES "${AMIGA_NDK_INCLUDE}")
endif()

# ------------------------------------------------------- vendored libraries --

file(GLOB THREADX_SOURCES "${AMINETXDUO_THREADX}/common/src/*.c")
file(GLOB NETXDUO_SOURCES "${AMINETXDUO_NETXDUO}/common/src/*.c")

if(NOT AMINETXDUO_IPV6)
    # Keep the IPv6 objects out of the floor build (~30 KB, docs/RESEARCH.md §5.4).
    list(FILTER NETXDUO_SOURCES EXCLUDE REGEX "/(nx_icmpv6|nx_ipv6|nx_nd)_")
endif()

add_library(threadx STATIC ${THREADX_SOURCES})
target_include_directories(threadx PUBLIC
    ${AMINETXDUO_PORT_INCLUDES} "${AMINETXDUO_THREADX}/common/inc")

add_library(netxduo STATIC ${NETXDUO_SOURCES})
target_include_directories(netxduo PUBLIC
    ${AMINETXDUO_CORE_INCLUDES})
target_link_libraries(netxduo PUBLIC threadx)

# NetX Duo add-ons we actually use.
add_library(netxduo_addons STATIC
    "${AMINETXDUO_NETXDUO}/addons/dhcp/nxd_dhcp_client.c"
    "${AMINETXDUO_NETXDUO}/addons/dns/nxd_dns.c"
    "${AMINETXDUO_NETXDUO}/addons/auto_ip/nx_auto_ip.c")
target_include_directories(netxduo_addons PUBLIC
    ${AMINETXDUO_CORE_INCLUDES}
    "${AMINETXDUO_NETXDUO}/addons/dhcp"
    "${AMINETXDUO_NETXDUO}/addons/dns"
    "${AMINETXDUO_NETXDUO}/addons/auto_ip")
target_link_libraries(netxduo_addons PUBLIC netxduo)

# --------------------------------------------------------- our components ---

add_subdirectory(port/threadx-amiga)
add_subdirectory(port/netxduo-amiga)
add_subdirectory(src/common)
add_subdirectory(src/config)
add_subdirectory(src/sana2)
add_subdirectory(src/bsdsocket)
add_subdirectory(src/usergroup)

if(AMINETXDUO_TESTS)
    add_subdirectory(tests)
endif()

# ------------------------------------------------------------------ report --

message(STATUS "AmiNetXDuo configuration:")
message(STATUS "  arch flags .... ${AMIGA_ARCH_FLAGS}")
message(STATUS "  IPv6 .......... ${AMINETXDUO_IPV6}")
message(STATUS "  TLS ........... ${AMINETXDUO_TLS}")
message(STATUS "  BPF ........... ${AMINETXDUO_BPF}")
