cmake_minimum_required(VERSION 3.20)

# The single source of truth for the product version.  Nothing else in the
# tree spells one out: cmake/AmiNetXDuoVersion.cmake takes this number,
# compounds it with the NetX Duo and ThreadX versions it reads out of
# third_party/, and generates include/aminetxduo/version.h.  Release tags are
# plain vX.Y.Z.
#
# 0.x on purpose: nothing has run on real hardware yet.
project(AmiNetXDuo VERSION 0.20.1 LANGUAGES C ASM)

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
# and it silently swallowed the first attempt at this change too, the
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

# No CMAKE_BUILD_TYPE means no -O at all, and that is a 45% throughput cliff.
#
# CMake adds nothing to CMAKE_C_FLAGS when the build type is empty, so a
# configure line that omits it compiles the whole tree at -O0 except the few
# files AMINETXDUO_HOT_O2 marks.  bsdsocket.library comes out 504,904 bytes
# instead of 308,248, and tests/compare/run-compare.sh -w bench on the A1200
# profile reads 291 KB/s over the wire and 362 on loopback against 425 and 500
# for the same commit built Release.  Roadshow, being a fixed binary, is
# unaffected, so the arm that moves is ours and the run looks like a defect in
# this stack.
#
# .github/workflows/{release,emulator}.yml and tools/ci.sh all pass Release, so
# nothing shipped was ever built this way; what was built this way is every
# measurement taken from a hand-configured tree.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release
        CACHE STRING "Build type (Release, Debug, RelWithDebInfo, MinSizeRel)"
        FORCE)
    message(STATUS "No CMAKE_BUILD_TYPE given; using Release")
endif()

# ---------------------------------------------------------------- options ---

option(AMINETXDUO_IPV6 "Build the IPv6 dual stack"                    ON)

# AMINETXDUO_IPV6 is GLOBAL, deliberately, and must stay that way.
#
# nx_user.h turns it into NX_DISABLE_IPV6 (or not), and NX_DISABLE_IPV6 decides
# FEATURE_NX_IPV6, which changes the layout of NX_IP, NX_INTERFACE, NX_PACKET
# and NX_TCP_SOCKET. A single translation unit compiled with a different answer
# from the rest of the tree does not fail to link, it reads the wrong offsets
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
#   * too slow, a handshake was 185 s.  That figure was a loopback total
#     dominated by the SERVER half, which a client never performs; the client
#     cost is ~7 s and resumption makes a repeat connection ~0.6 s.
#   * too big, the pair is ~520 KB.  Not a constraint on a 4 MB machine.
#   * nothing uses it, `fetch` does.
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
# ON EVERYWHERE, including the 68000.  It was off there, and that followed a
# decision the project had already taken.  §9's M9 gate benchmarked a real
# TLS 1.2 handshake on the 68020 floor and concluded TLS is not viable there:
# 185.5 s client-and-server on one CPU, 158.0 s for the RSA-2048 private
# operation alone, ~13-20 s derived for a client-only handshake, and the
# recorded conclusion that offload is the realistic path.  That was measured
# on a 7 MHz part, before c68k_prim_mulw.S: the limb multiply-accumulate the
# handshake spends half its time in is four MULU.W, which a 68000 has, and
# assembles for -m68000.  An RSA-2048 TLS 1.3 handshake at 13 MHz measures
# 60 s, down from 232 s before that assembly existed.
#
# 60 s is slow.  It is not our decision to make for somebody: the archive
# carries a binary for every processor and the installer picks the one this
# machine can run, and nothing is withheld for being slow.  The P-256 leg is
# a separate open defect, see docs/BACKLOG.md.
set(_aminetxduo_tls_default ON)
option(AMINETXDUO_TLS  "Build tls.library and its trust store"        ${_aminetxduo_tls_default})
option(AMINETXDUO_BPF  "Build the bpf_* raw path and filter VM"       ON)

# The SANA-II receive probe: how many CMD_READs the device still held at each
# wake, and whether the TCP sequence space is already holed where the frame is
# handed to NetX Duo. Off, and a measurement build only, it costs two
# ReadEClock() calls per drain and reports through the serial log.
option(AMINETXDUO_RXPROBE "Instrument the SANA-II receive path"       OFF)
if(AMINETXDUO_RXPROBE)
    add_compile_definitions(AMINETXDUO_RXPROBE)
endif()
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
# AMINETXDUO_BPF is, the netstack's mDNS attachment and the resolver's
# ".local" branch are compiled out without it, and two objects that disagreed
# about whether the module exists would disagree about netstack_resolve().
option(AMINETXDUO_MDNS "Build the mDNS responder and .local resolver" ON)
if(AMINETXDUO_MDNS)
    add_compile_definitions(AMINETXDUO_MDNS)
endif()

# The ARexx host on the AMITCP port: the QUERY/SET variable space and the
# process that services it.  Off, netstack.c creates the port as a bare public
# MsgPort instead, so `WaitForPort AMITCP` in S:User-Startup still returns and
# a Shell command can still see that a stack is running, that part is the
# convention every stack since AmiTCP has followed and is not optional.
#
# What goes with it is the 8 KB process stack, which is the part that matters
# on a 1 MB machine, and the answer to a script that sends commands: with no
# host servicing the port, `ADDRESS AMITCP` waits for a reply that never
# comes.  31 of the 2,149 archives in docs/RESEARCH.md 75 do that, AmiTCP's
# own stopnet and netstat among them, so this is off only in the floor tier,
# where those are not installed.
option(AMINETXDUO_AREXX "Build the ARexx host on the AMITCP port" ON)
if(AMINETXDUO_AREXX)
    add_compile_definitions(AMINETXDUO_AREXX)
endif()

# The serial diagnostic log: 136 format strings reaching RawPutChar().
#
# OFF in every shipping build, this one included.  Nothing on a normal machine
# captures the serial port, so the strings are written and never read, and
# they are the largest single thing a build can drop, 12,820 bytes on the
# 68000 floor tier, more than the ARexx host.  Off,
# include/aminetxduo/compat.h compiles AMI_ERROR/WARN/INFO to a branch the
# optimiser removes, arguments still type-checked.
#
# -DAMINETXDUO_LOG=ON is how a build for a bug report is made: a serial
# capture is the only record of what the stack did before it stopped, so
# reproducing on a logging build is the first step of any investigation that
# needs one.
option(AMINETXDUO_LOG "Build the serial diagnostic log" OFF)
if(AMINETXDUO_LOG)
    add_compile_definitions(AMINETXDUO_LOG)
endif()

# The TCP: AmigaDOS handler, a socket opened as a file handle, so a program
# that speaks DOS files speaks the network without knowing this library
# exists, and `Type TCP:host/port` works from the Shell.  AmiTCP and Roadshow
# both shipped one.
#
# Off, bsdsocket.library loses the handler process and its file-handle
# machinery: 4,976 bytes of text, and one fewer Process holding no OpenCnt
# reference for bsd_lib_expunge() to decline over.  Off in the floor tier,
# where the programs that reach for a device rather than a socket are not
# installed.
# Symbols are stripped from every binary that ships: the commands, the three
# libraries, and the profiler in Developer/.  -DAMINETXDUO_KEEP_SYMBOLS=ON keeps
# them, which is what a debugging session wants; the profiler does not need them,
# since it reads the linker map and the object files.  ssh comes from
# clients/dropbear/build.sh, outside this tree, and strips there (-k keeps).
option(AMINETXDUO_KEEP_SYMBOLS "Keep the symbol table in the built binaries" OFF)

option(AMINETXDUO_TCPDEVICE "Build the TCP: AmigaDOS handler" ON)
if(AMINETXDUO_TCPDEVICE)
    add_compile_definitions(AMINETXDUO_TCPDEVICE)
endif()

# Multicast group membership, both families: nx_igmp_enable() and the five
# IPPROTO_IP socket options, plus NX_ENABLE_IPV6_MULTICAST and the five
# IPPROTO_IPV6 ones in an AMINETXDUO_IPV6 build.  On by default, because the
# programs this stack exists to run, SSDP/UPnP clients, a ported mDNS, open
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
# so ami_sana2_open() never returns, verified under FS-UAE, 2026-07-25. Raw
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
# use it.  OFF, and it stays OFF, it is a tool for answering where the 78%
# of a transfer that tests/perf/perf_test.c cannot account for actually goes,
# not something the library ships.  It adds nothing to src/.
option(AMINETXDUO_PROFILER "Build the PC sampling profiler in tests/perf/prof" OFF)

option(AMINETXDUO_NXCENSUS "Count and time the ThreadX bracket" OFF)
if(AMINETXDUO_NXCENSUS)
    add_compile_definitions(AMINETXDUO_NXCENSUS)
endif()

# Tag every ami_alloc() with the file and line it came from, and print what is
# still held when the stack tears down.  OFF, and byte-identical when off: the
# tags are string literals, and on this target string literals live in plain
# .text (the note at the head of src/tools/tool_devdiag.c), so a shipping
# binary would carry every one of them.
#
# ms_Live in aminetxduo/compat.h already says HOW MANY blocks are outstanding.
# This says WHICH, which is the question every leak found so far turned on.
# include/aminetxduo/alloccensus.h; the report is read by tools/alloc-census.sh.
option(AMINETXDUO_ALLOCCENSUS "Tag allocations and report the outstanding set" OFF)
if(AMINETXDUO_ALLOCCENSUS)
    add_compile_definitions(AMINETXDUO_ALLOCCENSUS)
endif()

# How many live allocations the census table holds.  Only meaningful with
# AMINETXDUO_ALLOCCENSUS; the report says `lost=` when it was not enough.
set(AMINETXDUO_ALLOCCENSUS_SLOTS "2048" CACHE STRING
    "Allocation census table size, a power of two")
if(AMINETXDUO_ALLOCCENSUS)
    add_compile_definitions(
        AMINETXDUO_ALLOCCENSUS_SLOTS=${AMINETXDUO_ALLOCCENSUS_SLOTS})
endif()

# Count how often the scheduling primitives are entered.  The PC sampler says
# where samples land; this says how many calls they landed in, and the two
# together give a cost per call.  OFF, and free when off, see the counter
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
# how tests/trace/ settled it, see the note on BSD_TCP_WINDOW in
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
# 7200, two hours, which is BSD's and is what ships.  It exists because a
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

# RFC 1323 window scaling.  The note in port/netxduo-amiga/inc/nx_user.h says
# what it needed before it could be turned on and what turning it on measured.
#
# Build with -DAMINETXDUO_TCP_WINDOW_SCALING=OFF to take it out.
#
# It is an option for the same reason AMINETXDUO_TCP_WINDOW above is: "what
# does it cost and what does it buy?" is answerable only by building libraries
# out of ONE tree that differ in this alone, and this is how those arms were
# taken.  It changes the layout of NX_TCP_SOCKET, so it cannot be a per-target
# define.
option(AMINETXDUO_TCP_WINDOW_SCALING "Offer the RFC 1323 window scale option" ON)
if(AMINETXDUO_TCP_WINDOW_SCALING)
    add_compile_definitions(AMINETXDUO_TCP_WINDOW_SCALING)
endif()

# RFC 1323 section 3 timestamps, on every segment once both SYNs carry the
# option.  Also a layout change to NX_TCP_SOCKET, so also whole-tree.  It costs
# twelve bytes of every segment that carries data -- MSS 1460 becomes 1448 on a
# 1500-byte link -- and about four per cent of the write rate on an A1200.  The
# read rate, which is the figure tests/perf/run-fitzbench.sh compares on, does
# not move.
#
# Build with -DAMINETXDUO_TCP_TIMESTAMP=OFF to take it out.
option(AMINETXDUO_TCP_TIMESTAMP "Offer the RFC 1323 timestamps option" ON)
if(NOT AMINETXDUO_TCP_TIMESTAMP)
    add_compile_definitions(AMINETXDUO_TCP_TIMESTAMP_OFF)
endif()

# RFC 2018 selective acknowledgment on the receive side.  Also a layout change
# to NX_TCP_SOCKET, so also whole-tree.  Both arms of a with/without
# measurement come out of one tree the same way as the option above.
option(AMINETXDUO_TCP_SACK "Report out-of-order data in RFC 2018 SACK blocks" ON)
if(NOT AMINETXDUO_TCP_SACK)
    add_compile_definitions(AMINETXDUO_TCP_SACK_OFF)
endif()

# RFC 6298 round-trip time estimation, so the retransmission timer starts from
# a measured base rather than a fixed second.  Another NX_TCP_SOCKET layout
# change, so whole-tree for the same reason as the two above.
option(AMINETXDUO_TCP_RTT "Estimate the retransmission timeout per RFC 6298" ON)
if(NOT AMINETXDUO_TCP_RTT)
    add_compile_definitions(AMINETXDUO_TCP_RTT_OFF)
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
# include path, ThreadX and NetX Duo pick their port up by plain #include.
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
# two libraries built from one tree, differing in this one file, and it is
# the way to bisect a suspected checksum bug from the outside.
option(AMINETXDUO_NET68K_CHECKSUM
       "Replace NetX Duo's IP checksum with src/net68k's" ON)

if(AMINETXDUO_NET68K_CHECKSUM)
    list(FILTER NETXDUO_SOURCES EXCLUDE REGEX "/nx_ip_checksum_compute\\.c$")
endif()

# Verify receive checksums in the driver glue and tell NetX Duo, through
# NX_ENABLE_INTERFACE_CAPABILITY, not to walk them again.
#
# ami_sana2_copy_to_buff() reads every longword of the frame anyway, so it sums
# as it copies and the slot carries an answer nothing paid a second pass for;
# n68k_rx_verify_sum() checks the datagram against it.  A slot that could not
# sum (odd on one side, or no slot) falls back to n68k_rx_verify(), which walks
# and costs a second parse of the IP header.  src/net68k/n68k_rx_verify.c.
#
# It needs the fork's per-packet capability check: upstream NetX Duo consults
# only the interface flag, which would leave the glue answerable for frames it
# cannot verify.
#
# ON: this gates the fused copy-and-sum itself, not just the verify. Built OFF
# from a0f0c3e until now, which compiled the fusion out of every default build
# and left the receive path copying and then walking the same bytes again.
option(AMINETXDUO_RX_VERIFY
       "Verify receive checksums in the SANA-II glue" ON)

if(AMINETXDUO_RX_VERIFY)
    add_compile_definitions(AMINETXDUO_RX_VERIFY)
endif()

add_library(threadx STATIC ${THREADX_SOURCES})
target_include_directories(threadx PUBLIC
    ${AMINETXDUO_PORT_INCLUDES} "${AMINETXDUO_THREADX}/common/inc")

# ---------------------------------------------------------------------------
# The hot translation units, at -O2.
#
# Everything else stays -Os; see the note at the top of this file.  These were
# picked from a ranked profile of an A600 fitz transfer and from Profile WATCH
# over the copy routine, which named its callers.  -O2 mainly buys inlined
# small copies here: at -Os GCC calls memcpy for a 20-byte struct assignment,
# and nx_tcp_socket_packet_process does exactly that once per received packet.
#
# AMINETXDUO_HOT_O2=OFF puts them back at -Os, which is the comparison to make
# when deciding whether the size is worth it.
# ---------------------------------------------------------------------------
option(AMINETXDUO_HOT_O2 "Build the profiled hot paths at -O2" ON)

set(AMINETXDUO_HOT_SOURCES
    "${AMINETXDUO_NETXDUO}/common/src/nx_tcp_socket_packet_process.c"
    "${AMINETXDUO_NETXDUO}/common/src/nx_tcp_socket_send_internal.c"
    "${AMINETXDUO_NETXDUO}/common/src/nx_tcp_socket_state_ack_check.c"
    "${AMINETXDUO_NETXDUO}/common/src/nx_tcp_packet_process.c"
    "${AMINETXDUO_NETXDUO}/common/src/nx_packet_data_append.c"
    "${AMINETXDUO_NETXDUO}/common/src/nx_packet_data_extract_offset.c"
    "${AMINETXDUO_NETXDUO}/common/src/nx_packet_allocate.c"
    "${AMINETXDUO_NETXDUO}/common/src/nx_packet_release.c")

# nx_tcp_socket_state_data_trim_front.c was in this list from 7fa4ad6 and does
# not exist; the function lives in nx_tcp_socket_state_data_check.c, so the
# entry warned into a configure log and built at -Os for as long as it was
# here.  Naming the real file costs 484 bytes and measures worse -- that
# function goes 49 samples to 59, wire 421/445 to 420/430 -- so the entry is
# gone rather than corrected.  The warning is fatal now.
#
# MEASURED AND NOT TAKEN, 2026-08-07.  With the receive fusion on the ranking
# is ours rather than NetX Duo's -- ami_sana2_rx_thread 3.8% against 1.8% for
# the largest NetX Duo entry -- so the obvious move was to add our sana2,
# transfer, select and rx_verify files, four ThreadX scheduler and mutex files,
# and two more NetX Duo ones.  Eleven files, +7444 bytes, A1200 wire:
#
#     wire 422/446 against 421/445, total samples 2476 against 2486
#
# Nothing, for 2.4% of the library.  The per-function counts moved (recv_iov
# +11, rx_verify_sum +12, mutex_put +18) with the total flat, which is code
# being inlined from one name into another and not work going away.
#
# Our files compile in subdirectories, so adding them needs the source
# property to carry a DIRECTORY, and that directory has to be added first --
# the list would have to be applied below add_subdirectory(), not here.  That
# is worth knowing if a future profile makes the case; this one did not.

if(AMINETXDUO_HOT_O2)
    foreach(_hot IN LISTS AMINETXDUO_HOT_SOURCES)
        if(EXISTS "${_hot}")
            # After the -Os in CMAKE_C_FLAGS_RELEASE, and the last one wins.
            set_source_files_properties("${_hot}" PROPERTIES COMPILE_OPTIONS "-O2")
        else()
            # Fatal, not a warning.  A name that does not resolve builds the
            # file at -Os and says so in a configure log, which is indis-
            # tinguishable from the list working.
            message(FATAL_ERROR "hot source not found, the list is stale: ${_hot}")
        endif()
    endforeach()
endif()

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

# -ffunction-sections ON THE VENDORED ARCHIVES ONLY, so the --gc-sections the
# three libraries now link with has something to collect.
#
# A pulled archive member is kept whole, and the core's one-function-per-file
# convention already makes that granular enough.  The add-ons do not follow it:
# nxd_mdns.c, nxd_dhcp_client.c and nxd_dns.c are one translation unit each for
# a whole protocol, and bsdsocket.library reaches a fraction of every one of
# them.  Splitting those three is where the saving is -- nxd_dhcp_client 2,176
# bytes, nxd_mdns 1,960, nxd_dns 1,484, nx_utility 1,056.
#
# NOT on src/, measured: our own objects are all reachable, so the flag buys
# padding, and -fno-optimize-sibling-calls gives up the tail calls that pay for
# themselves.  Adding src/netstack, src/sana2 and src/bpf to this list puts
# 1,316 bytes BACK on bsdsocket.library; the tree-wide version costs 3,348.
# 68000-minimal, the drawer with the least room, gains the most: 249,488 to
# 239,540, 9,948 bytes and 3.99%, against 2.81% for the default build.
#
# NOT -fdata-sections: it isolates a $VER: tag so nothing relocates to it and
# the collector takes it, which is what cmake/check-version-tag.cmake exists to
# catch.  The vendored code carries no version tags and would be safe, but 168
# bytes is not worth a flag that means something different two directories away.
#
# -fno-optimize-sibling-calls TRAVELS WITH -ffunction-sections, ALWAYS -- see
# src/config/CMakeLists.txt for the reboot that rule was written from.
foreach(_vendored threadx netxduo netxduo_addons)
    target_compile_options(${_vendored} PRIVATE
                           -ffunction-sections -fno-optimize-sibling-calls)
endforeach()

if(AMINETXDUO_MDNS)
    # The mDNS module's tunables, set HERE and PUBLIC rather than in
    # port/netxduo-amiga/inc/nx_user.h, because they are the add-on's knobs and
    # not the core's, nx_user.h changes the layout of NX_IP and NX_PACKET for
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

        # Three vendored files are dropped from their glob so that ours are
        # what the linker resolves to. Nothing about that is visible from
        # third_party/, so an upstream fix to one of them can sit in the tree
        # with no effect on the binary, which is how the shared TLS record
        # block buffer stayed in the shipped record path after the submodule
        # was fixed. This is the alarm for the next one.
        add_test(NAME shadowed_sources
                 COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tools/check-shadowed-sources.sh"
                         "${CMAKE_CURRENT_SOURCE_DIR}")

        # A forward-declared struct in bsdsocket_vectors.h makes GCC drop the
        # __asm() register annotations of every vector that names it, with no
        # diagnostic at any warning level. if_freenameindex() read its argument
        # off the stack for as long as one was there.
        add_test(NAME vector_abi
                 COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tools/check-vector-abi.sh"
                         "${CMAKE_CURRENT_SOURCE_DIR}")
    endif()
endif()

# Every linked AmigaOS image is checked for the mis-resolved 32-bit PC-relative
# branch this toolchain emits from a cross-section tail call.  It is a POST_BUILD
# check because the wrong address exists only in the linked image, the object
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

# Applied to every image rather than to the ones that remembered to ask.  Four
# CMakeLists called the function above and twenty-four others link m68k
# executables without it, smoke, bracket and soak among them, and those are
# the harnesses that say whether the machine survived something.  A branch into
# the middle of a function reboots an A1200 wherever it lands, and the four that
# were covered were the four that had already been bitten.
#
# Overriding add_executable rather than editing twenty-four files: the check is
# only worth having if a harness added next month cannot be added without it.
# The old command is available as _add_executable.  IMPORTED and ALIAS targets
# have no file to check.
function(add_executable name)
    _add_executable(${name} ${ARGN})
    if(NOT "IMPORTED" IN_LIST ARGN AND NOT "ALIAS" IN_LIST ARGN)
        aminetxduo_check_pcrel(${name})
    endif()
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
# is the only way to see what this stack costs on real hardware, no emulator
# above a 68020 has a usable cycle model.  tools/profiler/CMakeLists.txt.
add_subdirectory(tools/profiler)

if(AMINETXDUO_TESTS)
    add_subdirectory(tests)
    # Diagnostic probes, built, never auto-run.  Two of them crash on
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
message(STATUS "  RX probe ...... ${AMINETXDUO_RXPROBE}")
message(STATUS "  mDNS .......... ${AMINETXDUO_MDNS}")
message(STATUS "  multicast ..... ${AMINETXDUO_MULTICAST}")
message(STATUS "  ARexx host .... ${AMINETXDUO_AREXX}")
message(STATUS "  serial log .... ${AMINETXDUO_LOG}")
message(STATUS "  TCP: device ... ${AMINETXDUO_TCPDEVICE}")
