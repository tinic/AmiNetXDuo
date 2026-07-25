cmake_minimum_required(VERSION 3.20)
project(AmiNetXDuo C ASM)

# ---------------------------------------------------------------- options ---

option(AMINETXDUO_IPV6 "Build the IPv6 dual stack"                    OFF)
option(AMINETXDUO_TLS  "Build the nx_secure TLS library (68030+)"     OFF)
option(AMINETXDUO_BPF  "Build the bpf_* raw path and filter VM"       ON)
option(AMINETXDUO_TESTS "Build the on-Amiga test harnesses"           ON)

# The SANA-II raw-framing probe posts a CMD_READ with SANA2IOF_RAW and takes it
# straight back with AbortIO(). Commodore's a2065.device 2.16 does not abort it,
# so ami_sana2_open() never returns -- verified under FS-UAE, 2026-07-25. Raw
# framing is off by default anyway (AMI_SANA2_RAW_DEFAULT), so the probe buys
# nothing but a hang; leave it off unless you are working on the raw path.
option(AMINETXDUO_SANA2_PROBE_RAW "Probe SANA-II devices for raw framing" OFF)
if(NOT AMINETXDUO_SANA2_PROBE_RAW)
    add_compile_definitions(AMI_SANA2_PROBE_RAW=0)
endif()

# ami_log() verbosity: 0 = errors only ... 4 = trace. Empty keeps compat.c's
# own default (WARN in release, DEBUG when AMINETXDUO_DEBUG is set).
set(AMINETXDUO_LOG_LEVEL "" CACHE STRING "Override ami_log() verbosity (0..4)")
if(NOT AMINETXDUO_LOG_LEVEL STREQUAL "")
    add_compile_definitions(AMINETXDUO_LOG_LEVEL=${AMINETXDUO_LOG_LEVEL})
endif()

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

# Host builds register their parser/VM tests with add_test() in the component
# directories. enable_testing() has to be called HERE as well, or the build
# root's CTestTestfile.cmake never descends into them and `ctest` from the top
# reports "No tests were found" even though each subdirectory has some.
if(NOT CMAKE_CROSSCOMPILING)
    enable_testing()
endif()

add_subdirectory(port/threadx-amiga)
add_subdirectory(port/netxduo-amiga)
add_subdirectory(src/common)
add_subdirectory(src/config)
add_subdirectory(src/sana2)
add_subdirectory(src/mbuf)
if(AMINETXDUO_BPF)
    add_subdirectory(src/bpf)
endif()
add_subdirectory(src/netstack)
add_subdirectory(src/bsdsocket)
add_subdirectory(src/usergroup)
add_subdirectory(src/tools)

if(AMINETXDUO_TESTS)
    add_subdirectory(tests)
endif()

# ------------------------------------------------------------------ report --

message(STATUS "AmiNetXDuo configuration:")
message(STATUS "  arch flags .... ${AMIGA_ARCH_FLAGS}")
message(STATUS "  IPv6 .......... ${AMINETXDUO_IPV6}")
message(STATUS "  TLS ........... ${AMINETXDUO_TLS}")
message(STATUS "  BPF ........... ${AMINETXDUO_BPF}")
