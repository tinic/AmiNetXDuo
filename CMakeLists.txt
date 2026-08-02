cmake_minimum_required(VERSION 3.20)

# The single source of truth for the product version.  Nothing else in the
# tree spells one out: cmake/AmiNetXDuoVersion.cmake takes this number,
# compounds it with the NetX Duo and ThreadX versions it reads out of
# third_party/, and generates include/aminetxduo/version.h.  Release tags are
# plain vX.Y.Z.
#
# 0.x on purpose: nothing has run on real hardware yet.
project(AmiNetXDuo VERSION 0.16.6 LANGUAGES C ASM)

# -Os, AFTER project(), because nothing earlier can make it stick.
#
# cmake/toolchain-m68k-amigaos.cmake sets CMAKE_C_FLAGS_RELEASE_INIT, and that
# is NOT enough: CMake's Compiler/GNU module runs after the toolchain file and
# APPENDS its own "-O3 -DNDEBUG", so the flags come out as
#
#     -Os -DNDEBUG -O3 -DNDEBUG
#
# and the last -O wins. That is exactly how this project spent a year
# believing it built at -O2 while every published figure was measured at -O3,
# and it silently swallowed the first attempt at this change too -- the
# libraries came out 0.1% smaller, which was the giveaway.
#
# Setting the real variable here, after the language is enabled, is the one
# place the answer cannot be appended to. Check it rather than trust it:
#
#     grep C_FLAGS build/<dir>/**/flags.make
#
# The rationale for -Os over -O3 is in the toolchain file and RESEARCH 57.
set(CMAKE_C_FLAGS_RELEASE "-Os -DNDEBUG"
    CACHE STRING "C flags for Release builds" FORCE)

# ---------------------------------------------------------------- options ---

option(AMINETXDUO_IPV6 "Build the IPv6 dual stack"                    ON)

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
#   * nothing uses it -- `fetch` does.
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
#
# OFF BY DEFAULT ON A 68000, and this follows a decision the project has
# already taken rather than making a new one.  §9's M9 gate benchmarked a real
# TLS 1.2 handshake on the 68020 floor and concluded TLS is not viable there:
# 185.5 s client-and-server on one CPU, 158.0 s for the RSA-2048 private
# operation alone, ~13-20 s derived for a client-only handshake, and the
# recorded conclusion that offload is the realistic path.  A 7 MHz 68000 on a
# 16-bit bus is around a quarter of that 14 MHz 68020 before accounting for
# having no 32-bit multiply at all, and the hand-written limb assembly cannot
# run there either.  Shipping tls.library for a 68000 would ship the number
# §9 already rejected, several times over.
#
# It is an option, not a wall: -DAMINETXDUO_TLS=ON with AMINETXDUO_CPU=68000
# configures, builds and links.  Nobody should wait for it.
if(AMINETXDUO_CPU STREQUAL "68000")
    set(_aminetxduo_tls_default OFF)
else()
    set(_aminetxduo_tls_default ON)
endif()
option(AMINETXDUO_TLS  "Build tls.library and its trust store"        ${_aminetxduo_tls_default})
option(AMINETXDUO_BPF  "Build the bpf_* raw path and filter VM"       ON)
if(AMINETXDUO_BPF)
    # Global, because the two SANA-II taps and the netstack's capture
    # attachment are compiled out without it and the bsdsocket LVOs answer
    # ENOSYS. One definition for every translation unit, so no two objects
    # can disagree about whether the taps exist.
    add_compile_definitions(AMINETXDUO_BPF)
endif()

# mDNS (RFC 6762).  On by default, and it pairs with the RFC 3927 link-local
# work: a machine that fell back to 169.254.x.x has an address nothing on the
# network can name, which is the hole this fills.  Global for the same reason
# AMINETXDUO_BPF is -- the netstack's mDNS attachment and the resolver's
# ".local" branch are compiled out without it, and two objects that disagreed
# about whether the module exists would disagree about netstack_resolve().
option(AMINETXDUO_MDNS "Build the mDNS responder and .local resolver" ON)
if(AMINETXDUO_MDNS)
    add_compile_definitions(AMINETXDUO_MDNS)
endif()

# Multicast group membership, both families: nx_igmp_enable() and the five
# IPPROTO_IP socket options, plus NX_ENABLE_IPV6_MULTICAST and the five
# IPPROTO_IPV6 ones in an AMINETXDUO_IPV6 build.  On by default, because the
# programs this stack exists to run -- SSDP/UPnP clients, a ported mDNS -- open
# their own multicast sockets and get ENOPROTOOPT without it.  Global:
# src/netstack/ calls nx_igmp_enable(), port/netxduo-amiga/inc/nx_user.h reads
# this to size NX_IP, and src/bsdsocket/ carries the per-socket state; all
# three must agree about whether the membership exists.
#
# The v6 half joins without MLD, which there is none of in the vendored NetX
# Duo.  port/netxduo-amiga/inc/nx_user.h says what that does and does not buy.
#
# Off is the floor tier's answer (dist/make-dist.sh's 68000-minimal drawer).
option(AMINETXDUO_MULTICAST "Build multicast membership (IGMP, IPv6 groups)" ON)
if(AMINETXDUO_MULTICAST)
    add_compile_definitions(AMINETXDUO_MULTICAST)
endif()

option(AMINETXDUO_TESTS "Build the on-Amiga test harnesses"           ON)

# The SANA-II raw-framing probe posts a CMD_READ with SANA2IOF_RAW and takes it
# straight back with AbortIO(). Commodore's a2065.device 2.16 does not abort it,
# so ami_sana2_open() never returns -- verified under FS-UAE, 2026-07-25. Raw
# framing is off by default anyway (AMI_SANA2_RAW_DEFAULT), so the probe buys
# nothing but a hang; leave it off unless you are working on the raw path.
option(AMINETXDUO_SANA2_PROBE_RAW "Probe SANA-II devices for raw framing" OFF)
if(AMINETXDUO_SANA2_PROBE_RAW)
    add_compile_definitions(AMI_SANA2_PROBE_RAW=1)
else()
    add_compile_definitions(AMI_SANA2_PROBE_RAW=0)
endif()

# Count the ThreadX brackets bsdsocket.library takes and time them, reporting
# both to the serial log when a base closes.  OFF: it puts two ReadEClock()
# calls around the thing it is measuring.  It exists so that "how many brackets
# does one `fetch` cost, and what fraction of the wall clock are they?" can
# be answered by measurement rather than by arithmetic on a throughput delta --
# see the census note in src/bsdsocket/netx_call.c.
# Build tests/perf/prof: a timer-driven PC sampler and the two binaries that
# use it.  OFF, and it stays OFF -- it is a tool for answering where the 78%
# of a transfer that tests/perf/perf_test.c cannot account for actually goes,
# not something the library ships.  It adds nothing to src/.
option(AMINETXDUO_PROFILER "Build the PC sampling profiler in tests/perf/prof" OFF)

option(AMINETXDUO_NXCENSUS "Count and time the ThreadX bracket" OFF)
if(AMINETXDUO_NXCENSUS)
    add_compile_definitions(AMINETXDUO_NXCENSUS)
endif()

# Count how often the scheduling primitives are entered.  The PC sampler says
# where samples land; this says how many calls they landed in, and the two
# together give a cost per call.  OFF, and free when off -- see the counter
# block in port/threadx-amiga/inc/tx_port.h.
option(AMINETXDUO_SCHEDCOUNT "Count the scheduling primitive calls" OFF)
if(AMINETXDUO_SCHEDCOUNT)
    add_compile_definitions(AMINETXDUO_SCHEDCOUNT)
endif()

# Keep the calling task's TX_THREAD between brackets instead of building and
# deleting one per call.  ON, and the OFF arm is the control: two libraries out
# of one tree differing in this one decision, which is how the before and after
# were measured.  src/bsdsocket/netx_call.c has the mechanism and the cost.
option(AMINETXDUO_NXCACHE "Cache the ThreadX adoption per task" ON)
if(NOT AMINETXDUO_NXCACHE)
    add_compile_definitions(AMINETXDUO_NXCACHE=0)
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

# Randomise the IP identification field.  OFF, and measured rather than assumed:
# NetX Duo draws it from NX_RAND once per transmitted datagram, NX_RAND here is
# a SHA-256 DRBG, and that is 5% of loopback throughput on the floor target.
# port/netxduo-amiga/inc/nx_user.h has the arms and says what ships instead.
# docs/RESEARCH.md 29.4.
option(AMINETXDUO_IP_ID_RANDOMIZATION "Randomise the IPv4 ID field" OFF)
if(AMINETXDUO_IP_ID_RANDOMIZATION)
    add_compile_definitions(AMINETXDUO_IP_ID_RANDOMIZATION)
endif()

# TCP keepalive's idle timer, in SECONDS.  Empty keeps NetX Duo's default of
# 7200 -- two hours, which is BSD's and is what ships.  It exists because a
# two-hour timer cannot be observed inside an emulator run: the keepalive arm
# in docs/RESEARCH.md 29 is built with 5 so that tests/tcpdrill can watch the
# probe leave.  nx_tcp.h guards the macro with #ifndef, so this reaches it
# without nx_user.h having to hold a number nobody ships.
set(AMINETXDUO_TCP_KEEPALIVE_INITIAL "" CACHE STRING
    "TCP keepalive idle timer in seconds (default 7200)")
if(NOT AMINETXDUO_TCP_KEEPALIVE_INITIAL STREQUAL "")
    add_compile_definitions(
        NX_TCP_KEEPALIVE_INITIAL=${AMINETXDUO_TCP_KEEPALIVE_INITIAL})
endif()

# RFC 1323 window scaling.  OFF, and measured rather than assumed -- the note
# in port/netxduo-amiga/inc/nx_user.h says what it changes (nothing this stack
# can reach) and what it costs (NetX Duo's own guard against a window larger
# than this machine can carry).  docs/RESEARCH.md 28.2.
#
# It is an option for the same reason AMINETXDUO_TCP_WINDOW above is: "what
# does it cost and what does it buy?" is answerable only by building libraries
# out of ONE tree that differ in this alone, and this is how those arms were
# taken.  It changes the layout of NX_TCP_SOCKET, so it cannot be a per-target
# define.
option(AMINETXDUO_TCP_WINDOW_SCALING "Offer the RFC 1323 window scale option" OFF)
if(AMINETXDUO_TCP_WINDOW_SCALING)
    add_compile_definitions(AMINETXDUO_TCP_WINDOW_SCALING)
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
set(NETXDUO_ADDON_SOURCES
    "${AMINETXDUO_NETXDUO}/addons/dhcp/nxd_dhcp_client.c"
    "${AMINETXDUO_NETXDUO}/addons/dns/nxd_dns.c"
    "${AMINETXDUO_NETXDUO}/addons/auto_ip/nx_auto_ip.c")
set(NETXDUO_ADDON_INCLUDES
    "${AMINETXDUO_NETXDUO}/addons/dhcp"
    "${AMINETXDUO_NETXDUO}/addons/dns"
    "${AMINETXDUO_NETXDUO}/addons/auto_ip")

if(AMINETXDUO_MDNS)
    list(APPEND NETXDUO_ADDON_SOURCES "${AMINETXDUO_NETXDUO}/addons/mdns/nxd_mdns.c")
    list(APPEND NETXDUO_ADDON_INCLUDES "${AMINETXDUO_NETXDUO}/addons/mdns")
endif()

add_library(netxduo_addons STATIC ${NETXDUO_ADDON_SOURCES})
target_include_directories(netxduo_addons PUBLIC
    ${AMINETXDUO_CORE_INCLUDES}
    ${NETXDUO_ADDON_INCLUDES})
target_link_libraries(netxduo_addons PUBLIC netxduo)

if(AMINETXDUO_MDNS)
    # The mDNS module's tunables, set HERE and PUBLIC rather than in
    # port/netxduo-amiga/inc/nx_user.h, because they are the add-on's knobs and
    # not the core's -- nx_user.h changes the layout of NX_IP and NX_PACKET for
    # the whole tree, and nothing about mDNS should reach that far.  PUBLIC so
    # nxd_mdns.c and src/netstack/netstack_mdns.c cannot disagree about the
    # size of an NX_MDNS.
    #
    # NX_MDNS_ENABLE_IPV6 is deliberately NOT set even in the IPv6 build: the
    # v6 half wants MLD group membership this stack does not run, and the whole
    # point of the module here is that a machine is reachable by name over the
    # network it actually has.
    target_compile_definitions(netxduo_addons PUBLIC
        # RFC 6762 §9 renaming stops after this many collisions and reports
        # failure.  Four is plenty: past "amiga (4)" the answer is to set
        # HOSTNAME, not to keep counting.
        NX_MDNS_CONFLICT_COUNT=4)
endif()

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

# Every linked AmigaOS image is checked for the mis-resolved 32-bit PC-relative
# branch this toolchain emits from a cross-section tail call.  It is a POST_BUILD
# check because the wrong address exists only in the linked image -- the object
# file's relocation is well formed.  `ping` rebooted the machine on one of these.
# See cmake/check-pcrel-branches.cmake and docs/RESEARCH.md §25.
function(aminetxduo_check_pcrel target)
    if(NOT CMAKE_CROSSCOMPILING)
        return()
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}"
                "-DBINARY=$<TARGET_FILE:${target}>"
                "-DOBJDUMP=${CMAKE_OBJDUMP}"
                "-DNM=${CMAKE_NM}"
                -P "${CMAKE_SOURCE_DIR}/cmake/check-pcrel-branches.cmake"
        VERBATIM)
endfunction()

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

# Profile, the sampling profiler.  Built for every cross configuration and
# packed in the archive: it depends on nothing in this tree, it is useful to
# anyone with an Amiga and a program that is slower than they expected, and it
# is the only way to see what this stack costs on real hardware -- no emulator
# above a 68020 has a usable cycle model.  tools/profiler/CMakeLists.txt.
add_subdirectory(tools/profiler)

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
string(REPLACE ";" " " _amiga_arch_shown "${AMIGA_ARCH_FLAGS}")
message(STATUS "  CPU ........... ${AMINETXDUO_CPU} (${_amiga_arch_shown})")
message(STATUS "  IPv6 .......... ${AMINETXDUO_IPV6}")
message(STATUS "  TLS ........... ${AMINETXDUO_TLS}")
message(STATUS "  BPF ........... ${AMINETXDUO_BPF}")
message(STATUS "  mDNS .......... ${AMINETXDUO_MDNS}")
message(STATUS "  multicast ..... ${AMINETXDUO_MULTICAST}")
