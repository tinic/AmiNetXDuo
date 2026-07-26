cmake_minimum_required(VERSION 3.20)

# The single source of truth for the product version.  Nothing else in the
# tree spells one out: cmake/AmiNetXDuoVersion.cmake takes this number,
# compounds it with the NetX Duo and ThreadX versions it reads out of
# third_party/, and generates include/aminetxduo/version.h.  Release tags are
# plain vX.Y.Z.
#
# 0.x on purpose: nothing has run on real hardware yet.
project(AmiNetXDuo VERSION 0.2.0 LANGUAGES C ASM)

# ---------------------------------------------------------------- options ---

option(AMINETXDUO_IPV6 "Build the IPv6 dual stack"                    OFF)

# AMINETXDUO_IPV6 is GLOBAL, deliberately, and must stay that way.
#
# nx_user.h turns it into NX_DISABLE_IPV6 (or not), and NX_DISABLE_IPV6 decides
# FEATURE_NX_IPV6, which changes the layout of NX_IP, NX_INTERFACE, NX_PACKET
# and NX_TCP_SOCKET. A single translation unit compiled with a different answer
# from the rest of the tree does not fail to link -- it reads the wrong offsets
# out of a shared struct. It used to be set per-target on aminetxduo_sana2
# only, which meant the SANA-II shim saw a different NX_IP from the NetX Duo
# core it was driving.
if(AMINETXDUO_IPV6)
    add_compile_definitions(AMINETXDUO_IPV6)
endif()
# TLS: tls.library, the nx_secure/nx_crypto stack under it, and the trust store.
#
# ON by default.  It was off for a long time and the stated reason changed
# four times, which is itself the interesting part of the history:
#
#   * too slow -- a handshake was 185 s.  That figure was a loopback total
#     dominated by the SERVER half, which a client never performs; the client
#     cost is ~7 s and resumption makes a repeat connection ~0.6 s.
#   * too big -- the pair is ~520 KB.  Not a constraint on a 4 MB machine.
#   * nothing uses it -- `fetch` does, and so does curl through the vtls
#     backend in clients/curl.
#   * a chain of three or more crashes the machine.  THIS WAS WRONG and is
#     the one worth remembering: fs-uae was being killed by SIGPIPE on the
#     HOST when a slow handshake outlasted the peer's patience, and every
#     symptom of that read like a guru.  Withdrawn in docs/RESEARCH.md.
#
# What remains is a property of the machine rather than a defect: a FIRST
# handshake to a CDN-fronted host may not finish inside the far end's timeout
# at 14 MHz.  The connection fails cleanly, says so, and usually succeeds on
# the next attempt because the session is cached.  That is not a reason to
# ship without encryption.
option(AMINETXDUO_TLS  "Build tls.library and its trust store"        ON)
option(AMINETXDUO_BPF  "Build the bpf_* raw path and filter VM"       ON)
if(AMINETXDUO_BPF)
    # Global, because the two SANA-II taps and the netstack's capture
    # attachment are compiled out without it and the bsdsocket LVOs answer
    # ENOSYS. One definition for every translation unit, so no two objects
    # can disagree about whether the taps exist.
    add_compile_definitions(AMINETXDUO_BPF)
endif()
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

# PIN the TCP receive window at this many bytes, floor and ceiling together,
# instead of letting ami_bsd_tcp_window() size it from the packet pool and the
# live socket count.  Exists so that "is the window the limiter?" can be
# answered by building two libraries out of one tree and measuring, which is
# how tests/trace/ settled it -- see the note on BSD_TCP_WINDOW in
# src/bsdsocket/bsdsocket_internal.h.  Empty means pool-derived, which is what
# ships; 8192 reproduces the fixed window that shipped before it.
set(AMINETXDUO_TCP_WINDOW "" CACHE STRING "Pin the TCP receive window, bytes")
if(NOT AMINETXDUO_TCP_WINDOW STREQUAL "")
    add_compile_definitions(BSD_TCP_WINDOW=${AMINETXDUO_TCP_WINDOW}
                            BSD_TCP_WINDOW_CEILING=${AMINETXDUO_TCP_WINDOW})
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

# ---------------------------------------------------------------- version --
#
# Must come after the submodule paths above: it reads nx_api.h and tx_api.h,
# checks them against the pins it records, and generates
# ${CMAKE_BINARY_DIR}/include/aminetxduo/version.h.
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/AmiNetXDuoVersion.cmake")

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
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    # Generated <aminetxduo/version.h> lives here, alongside the hand-written
    # headers of the same prefix.
    "${CMAKE_BINARY_DIR}/include")

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

# src/net68k/ supplies _nx_ip_checksum_compute(), so the vendored object must
# not.  This is the mechanism for replacing a NetX Duo inner loop without
# editing third_party/: drop the file from the glob and every internal caller
# resolves to ours instead.  See src/net68k/n68k_checksum.c for what changes
# (the loop) and what deliberately does not (everything else).
#
# Turning the option OFF restores the vendored object and drops net68k's hook.
# That is how the before/after measurements in docs/RESEARCH.md were taken --
# two libraries built from one tree, differing in this one file -- and it is
# the way to bisect a suspected checksum bug from the outside.
option(AMINETXDUO_NET68K_CHECKSUM
       "Replace NetX Duo's IP checksum with src/net68k's" ON)

if(AMINETXDUO_NET68K_CHECKSUM)
    list(FILTER NETXDUO_SOURCES EXCLUDE REGEX "/nx_ip_checksum_compute\\.c$")
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

    # tools/version.sh answers the same questions as the generated header, by
    # its own parse of the same files, because CI has to be able to name an
    # artefact without configuring a build. Two implementations means they can
    # drift; this is what stops that, and it needs no build step.
    if(UNIX)
        add_test(NAME version_scheme
                 COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tools/version.sh"
                         --check "${CMAKE_BINARY_DIR}/include/aminetxduo/version.h")
    endif()
endif()

add_subdirectory(port/threadx-amiga)
add_subdirectory(port/netxduo-amiga)

# net68k before anything that links netxduo: it supplies a symbol the vendored
# core now leaves undefined, so netxduo carries it as a PUBLIC dependency and
# every consumer picks it up without listing it.
add_subdirectory(src/net68k)
if(AMINETXDUO_NET68K_CHECKSUM)
    target_link_libraries(netxduo PUBLIC aminetxduo_net68k)
endif()

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
if(AMINETXDUO_TLS)
    add_subdirectory(src/tls)
endif()
# After src/tls: crypto68k reuses aminetxduo_m68k_rt when that target exists.
add_subdirectory(src/crypto68k)

# tls.library. After src/crypto68k, because it names that target directly: the
# archives go on the link line as FILES rather than as CMake targets, so that
# nothing pulls `netxduo` in transitively (see src/tlslib/CMakeLists.txt).
if(AMINETXDUO_TLS)
    add_subdirectory(src/tlslib)
endif()
add_subdirectory(src/tools)

if(AMINETXDUO_TESTS)
    add_subdirectory(tests)
    # Diagnostic probes -- built, never auto-run.  Two of them crash on
    # purpose; see tools/smoke/CMakeLists.txt.
    add_subdirectory(tools/smoke)
endif()

# ------------------------------------------------------------------ report --

message(STATUS "AmiNetXDuo configuration:")
message(STATUS "  version ....... ${AMINETXDUO_VERSION_COMPOUND}")
message(STATUS "  built on ...... NetX Duo ${AMINETXDUO_NETXDUO_VERSION}, ThreadX ${AMINETXDUO_THREADX_VERSION}")
message(STATUS "  arch flags .... ${AMIGA_ARCH_FLAGS}")
message(STATUS "  IPv6 .......... ${AMINETXDUO_IPV6}")
message(STATUS "  TLS ........... ${AMINETXDUO_TLS}")
message(STATUS "  BPF ........... ${AMINETXDUO_BPF}")
