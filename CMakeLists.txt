cmake_minimum_required(VERSION 3.20)

# The single source of truth for the product version.  Nothing else in the
# tree spells one out: cmake/AmiNetXDuoVersion.cmake takes this number,
# compounds it with the NetX Duo and ThreadX versions it reads out of
# third_party/, and generates include/aminetxduo/version.h.  Release tags are
# plain vX.Y.Z.
#
# 0.x on purpose: nothing has run on real hardware yet.
project(AmiNetXDuo VERSION 0.26.2 LANGUAGES C ASM)

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

# -flto, for the cross build only.  Measured against the same sources and
# shipping strip settings on the pinned toolchain:
#
#     bsdsocket.library   352,960   was 395,328    -10.7%
#     tls.library         188,312   was 242,312    -22.4%
#     usergroup.library     7,172   was   7,684     -6.7%
#     anxnet.device        30,540   was  33,228     -8.1%
#
# anxnet.device used to be the one image that GREW under -flto, by 6.1%, and
# that was never an LTO effect: it was the only target built -O2 for the whole
# image, where at -O2 the inliner is aggressive rather than size-constrained.
# At -Os it behaves like the other three. src/netdev/CMakeLists.txt carries the
# measurement that decided the switch.
#
# crypto68k is IN, and the only -fno-lto left in a shipping build is on
# src/tlslib/tls_resume.c. It was held out twice on the way here, once for a
# late compiler-libcall bug and once for a second defect with a different
# signature; both are fixed. bsdsocket.library pays nothing either way,
# crypto68k not being linked there.
#
# THIS NEEDS A TOOLCHAIN THAT CAN DO IT, and until 2026-08 none existed. The
# published asset was built with --disable-plugins, so its ld answers
# `-plugin PLUGIN (ignored)' and -flto silently does nothing useful; and even
# with plugins on, five defects in bfd made it unusable -- ld segfaulting on
# any LTO object, the plugin's IR dummy contributing no symbols at all, an
# archive member never being pulled in, --gc-sections purging the whole link,
# and the LTO section length being guessed by stripping trailing zeros. All
# five are fixed in the pinned toolchain; tools/fetch-toolchain.sh is what
# guarantees you have it. A toolchain from anywhere else will fail loudly on
# the first one and silently on the fourth.
#
# THE TRAP, and the reason every shipped binary carries a -u below: nothing
# references an AmigaOS romtag. Exec finds it by scanning the loaded segment,
# so a whole-program build removes the romtag, the autoinit table, the LVO
# vector table and every vector -- and the link still succeeds, exit 0, no
# diagnostic. A 964-byte test library came out at 20 bytes that way. Naming
# the romtag with -Wl,-u, holds the entire chain, and each of the four
# targets does that in its own CMakeLists.txt beside the target it protects.
#
# The same blindness applies to any symbol whose only reference is in a
# top-level asm() string; src/common/crashguard.c has one, marked `used'.
#
# tools/analyze.sh turns AMINETXDUO_LTO off because -flto SILENTLY DISABLES
# -fanalyzer. The same
#    translation unit that reports a use-of-uninitialised-value on its own
#    reports nothing at all with -flto added -- no note, no warning. The
#    static-analysis stage would have gone quiet rather than red, and what
#    actually failed CI was an accident: `cat | grep -Wanalyzer` returning 1 on
#    an empty result under pipefail. Without that, it would have read green.
#
# crypto68k is held OUT of LTO again, for a SECOND defect, not the first one.
# The late compiler-libcall bug in src/common/ami_udivdi3.c is fixed and the
# release gate proves it -- every surviving helper has its own address and a
# real size. But with crypto68k under -flto, tls.library completes zero TLS
# 1.3 handshakes and https: fails with UNEXPECTED_EOF_WHILE_READING, which is
# a different signature from the old BAD_ECPOINT. src/crypto68k/CMakeLists.txt
# carries the evidence and the probe to chase it with.
option(AMINETXDUO_LTO "Build with -flto (cross builds only)" ON)

if(CMAKE_CROSSCOMPILING AND AMINETXDUO_LTO)
    string(APPEND CMAKE_C_FLAGS_RELEASE " -flto")

    # A 68020+ LTO link partitions one translation unit into several output
    # sections and can therefore create the same cross-section 32-bit sibling
    # branch as -ffunction-sections.  The pinned linker resolves that relocation
    # twelve bytes short.  The all-CPU/68000 build cannot encode bra.l and does
    # not pay for disabling otherwise useful tail calls.
    if(AMIGA_ARCH_FLAGS_STR MATCHES "(^| )(-m68020|-m68040|-m68060|-mcpu=68060)( |$)")
        string(APPEND CMAKE_C_FLAGS_RELEASE " -fno-optimize-sibling-calls")
    endif()

    set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE}"
        CACHE STRING "C flags for Release builds" FORCE)
    add_link_options(-flto)
endif()

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
#   * too big, the pair is 413 KB (installdrive.c:133).  True on the 1 MB
#     floor: dist/ ships a 68000-minimal drawer with TLS compiled out.
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
# a separate open defect.
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

# The serial diagnostic log: AMI_ERROR, AMI_WARN and AMI_INFO.
#
# OFF in every shipping build.  bsdsocket.library and anxnet.device stay
# RESIDENT for the life of the machine, and the sentences cost the library
# 27,948 bytes -- 7.2 per cent of it -- for a serial port that on almost every
# machine has nothing on the other end.  Measured 0.26.0: 389,376 with them,
# 361,428 without, anxnet.device 38,620 either way because src/netdev has no
# AMI_ call in it.
#
# 0.26.0 shipped them unconditionally on the argument that a user who has hit a
# fault must have something to send.  That argument holds and it is answered by
# the event ring below, which an image carries in BSS at no image cost and
# ShowNetStatus reads.  The serial tier is the second copy, in prose, in RAM.
#
# -DAMINETXDUO_LOG=ON is how a build for a bug report is made, and the `log`
# cross arm keeps it compiling.  AMINETXDUO_LOG_LEVEL is where the runtime
# level starts either way; ENV:ANXDLOGLEVEL still moves it (src/config/
# config_file.c).  AMI_DEBUG and AMI_TRACE stay behind AMINETXDUO_DEBUG.
option(AMINETXDUO_LOG "Build the serial diagnostic log" OFF)
if(AMINETXDUO_LOG)
    add_compile_definitions(AMINETXDUO_LOG)
endif()

# The event ring, which is what a shipped binary has instead of that log.  A
# code, an interface and a value per event, in BSS, with the sentences in
# ShowNetStatus.  Not an option: a diagnostic that can be switched off is a
# diagnostic that is off on the machine that needed it, which is the defect the
# serial log already has.
#
# The SIZE is the option.  An AmigaOS hunk records BSS as a length and no
# bytes, so this costs nothing in any image and 16 bytes of RAM per entry on
# the machine.  The floor is 8: a bring-up and the shutdown that follows it are
# eight events on a one-card machine, and a ring that cannot hold one sequence
# answers nothing.
set(AMINETXDUO_EVENT_RING "32" CACHE STRING
    "Events the diagnostic ring keeps (8 or more)")
if(AMINETXDUO_EVENT_RING LESS 8)
    message(FATAL_ERROR
        "AMINETXDUO_EVENT_RING=${AMINETXDUO_EVENT_RING} is below the floor of 8")
endif()
# Applied to src/common alone, where the ring is, and not with
# add_compile_definitions(): a define in every compile line of the tree changes
# the LTO options record of every object, and anxnet.device -- which does not
# link src/common and has no ring in it -- moved 8 bytes for it.  A build
# option that changes an image it does not belong to is a measurement nobody
# can trust afterwards.

# HOW MANY INTERFACES CAN BE ONLINE AT ONCE.  Four, which is what an A1200 with
# a PiStorm32 has: two conventional cards plus genet.device and wifipi.device
# from the accelerator.  port/netxduo-amiga/inc/nx_user.h carries the
# measurement -- 4,032 bytes of the one allocation the stack makes and 808 more
# of interface-list floor, against 2 -- and the argument for taking it on every
# machine.
#
# A SMALLER MACHINE CAN HAVE FEWER, and this is the knob rather than a patch:
#
#   cmake -DAMINETXDUO_MAX_INTERFACES=2 ...
#
# It is one number wearing two names -- NX_MAX_PHYSICAL_INTERFACES sizes the
# NX_INTERFACE array inside every NX_IP, AMI_CFG_MAX_ATTACHED sizes our own
# per-slot tables -- so both are set here together and src/netstack/netstack.c
# has a _Static_assert that they agree.  A user who defines one by hand gets a
# compile error rather than a wrong binary.
#
# add_compile_definitions() DELIBERATELY, against the caution written just
# above: this constant changes the layout of NX_IP, so a translation unit that
# saw a different value would index past the end of an array.  Every TU has to
# see the same number, which is exactly what a global define is for.  Nothing
# is defined at the default, so the shipped build's compile lines -- and its
# LTO options record -- are the bytes they were before this option existed.
set(AMINETXDUO_MAX_INTERFACES "4" CACHE STRING
    "Interfaces that can be online at once (1 or more; 4 ships)")
if(AMINETXDUO_MAX_INTERFACES LESS 1)
    message(FATAL_ERROR
        "AMINETXDUO_MAX_INTERFACES=${AMINETXDUO_MAX_INTERFACES} is below the\
 floor of 1: a stack with no interface slot cannot bring anything up")
endif()
if(NOT AMINETXDUO_MAX_INTERFACES EQUAL 4)
    add_compile_definitions(
        NX_MAX_PHYSICAL_INTERFACES=${AMINETXDUO_MAX_INTERFACES}
        AMI_CFG_MAX_ATTACHED=${AMINETXDUO_MAX_INTERFACES})
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

# ASan and UBSan over the host test tier.  The target has no MMU and no
# sanitizer runtime, so the lever is the host builds, which compile the real
# stack sources against the Linux/macOS shims.  Inert when cross-compiling for
# that reason; tools/ci.sh's sanitize stage is what turns it on.  A host whose
# compiler cannot link the ASan runtime for the requested flags (-m32 without
# a 32-bit libasan, macOS with -m32 at all) degrades to UBSan alone and says
# so; a compiler with neither is a hard error, because a sanitize build that
# sanitizes nothing would read as coverage.
option(AMINETXDUO_SANITIZE "Build the host test tier under ASan/UBSan" OFF)
if(AMINETXDUO_SANITIZE AND NOT CMAKE_CROSSCOMPILING)
    include(CheckCSourceCompiles)
    set(_san_flags -fsanitize=address,undefined -fno-sanitize-recover=all
                   -fno-omit-frame-pointer -g)
    set(CMAKE_REQUIRED_FLAGS "-fsanitize=address,undefined")
    check_c_source_compiles("int main(void){return 0;}" AMINETXDUO_SANITIZE_ASAN)
    unset(CMAKE_REQUIRED_FLAGS)
    if(NOT AMINETXDUO_SANITIZE_ASAN)
        set(CMAKE_REQUIRED_FLAGS "-fsanitize=undefined")
        check_c_source_compiles("int main(void){return 0;}" AMINETXDUO_SANITIZE_UBSAN)
        unset(CMAKE_REQUIRED_FLAGS)
        if(NOT AMINETXDUO_SANITIZE_UBSAN)
            message(FATAL_ERROR "AMINETXDUO_SANITIZE=ON but this compiler has"
                                " neither ASan nor UBSan for these flags")
        endif()
        set(_san_flags -fsanitize=undefined -fno-sanitize-recover=all
                       -fno-omit-frame-pointer -g)
        message(STATUS "sanitize: undefined only (no ASan runtime for these flags)")
    else()
        message(STATUS "sanitize: address,undefined")
    endif()
    add_compile_options(${_san_flags})
    add_link_options(${_san_flags})
elseif(AMINETXDUO_SANITIZE)
    message(STATUS "sanitize: inert in a cross build, the target has no runtime")
endif()

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
# docs/RESEARCH.md 33.4.
option(AMINETXDUO_IP_ID_RANDOMIZATION "Randomise the IPv4 ID field" OFF)
if(AMINETXDUO_IP_ID_RANDOMIZATION)
    add_compile_definitions(AMINETXDUO_IP_ID_RANDOMIZATION)
endif()

# TCP keepalive's idle timer, in SECONDS.  Empty keeps NetX Duo's default of
# 7200, two hours, which is BSD's and is what ships.  It exists because a
# two-hour timer cannot be observed inside an emulator run: the keepalive arm
# in docs/RESEARCH.md 33.1 is built with 5 so that tests/tcpdrill can watch the
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

# Acknowledge every Nth data segment instead of the byte-threshold policy.
# The policy that ships is the else arm of nx_tcp_socket_state_data_check.c:
# an acknowledgment leaves when half the receive buffer is outstanding, see
# the NX_TCP_ACK_EVERY_N_PACKETS note in port/netxduo-amiga/inc/nx_user.h for
# why a segment count was retired.  Empty means that policy; a number brings
# the vendored counter arm back with that N, which is how one tree measures
# the two against each other on a machine the X-Surf sweep never ran on.
set(AMINETXDUO_TCP_ACK_EVERY_N ""
    CACHE STRING "ACK every Nth data segment instead of the byte threshold")
if(NOT AMINETXDUO_TCP_ACK_EVERY_N STREQUAL "")
    add_compile_definitions(
        NX_TCP_ACK_EVERY_N_PACKETS=${AMINETXDUO_TCP_ACK_EVERY_N})
endif()

# Lazy SANA-II transmit-completion collection, and what ships.  The arm this
# replaced signalled a reader per completed CMD_WRITE; during a bulk receive
# that wake preempts the thread emitting the acknowledgment to collect a reply
# the next send's own reap walk would have collected anyway, measured at
# ~0.7 ms of exec glue and two scheduler transitions per acknowledgment on the
# A1200.  ON parks the reply port PA_IGNORE while sends flow and moves the
# quiet-link safety net, the lesson in sana2_tx.c's header, to a one-tick timer
# that bounds any completion's collection at ~20 ms.  Behaviour, not layout, so
# it could be per-target; whole-tree like its neighbours so one flag is one arm
# of an A/B.
#
# UNVERIFIED ON REAL HARDWARE AS OF 2026-08-27, AND THE ONE MACHINE THAT CAN
# SAY IS amiga-1200 (192.168.1.219), a PiStorm32-accelerated A1200.  What this
# option removes is CPU work -- exec glue and scheduler transitions -- and that
# machine's CPU is enormously faster than its chipset, its Gayle bus and its
# 3c589, so the cost this exists to remove is the cost that got cheap there.
# The emulated win may shrink or invert.  A measurement is running; until it
# reports, this default rests on emulation alone.
#
# WHAT MOVED THE DEFAULT.  A1200 at 68020 under Amiberry, bridged, three cards,
# three interleaved rounds of tests/perf/run-fitzbench.sh -k 512 -r 3 against
# playhouse4, ON and OFF alternating inside one block:
#
#   a2065          read 530.7 vs 498.7 KB/s  +6.4%   write +2.3%
#   ariadne2       read 442.0 vs 430.7 KB/s  +2.6%   write +2.2%
#   ne2000_pcmcia  read 466.5 vs 446.7 KB/s  +4.4%   write +1.2%
#
# Every round on every card is above its OFF neighbour, and the read figure is
# the one the project compares on.  Build with -DAMINETXDUO_TX_LAZY_COLLECT=OFF
# to take it out; the `pathswap` arm in tools/ci.sh is what compiles that side.
option(AMINETXDUO_TX_LAZY_COLLECT
       "Collect TX completions lazily, signalling only on a quiet link" ON)
if(AMINETXDUO_TX_LAZY_COLLECT)
    add_compile_definitions(AMINETXDUO_TX_LAZY_COLLECT)
endif()

# The green realm:
# every thread the stack creates becomes a coroutine inside the ONE master
# Exec Task, with a real m68k stack switch per handoff instead of an Exec
# Signal/Wait/dispatch round trip; the realm owns the stack's single idle
# Wait().  Adopted application callers keep their own Task and the baton
# protocol.  PROTOTYPE: whole-tree, default OFF, and every existing
# configuration builds the baton port unchanged when it is off.
#
# WHY THE DEFAULT DID NOT MOVE.  A1200 at 68020 under Amiberry, bridged,
# run-fitzbench.sh -k 512 -r 3, three rounds interleaved against the baton
# build inside one block:
#
#   a2065          read  495.0 vs 498.7  -0.7%   write  408.3 vs 455.7  -10.4%
#   ariadne2       read  428.7 vs 430.7  -0.5%   write  343.0 vs 373.7   -8.2%
#   ne2000_pcmcia  read  443.7 vs 446.7  -0.7%   write  362.0 vs 396.7   -8.7%
#
# The realm was live, not disengaged: the guest's own NetStat counts 2 baton
# transitions over a run where the baton build counts 84, so the Exec-level
# handoffs really did become in-realm stack switches.  They bought nothing on
# the read figure and cost 8-10% of the write figure on every card.
#
# WHAT IS MISSING BEFORE IT IS WORTH REVISITING, specifically:
#   - an account of where the write loss comes from.  The send path is the one
#     that hands off to a green reader per completed CMD_WRITE, and nothing
#     here says whether the loss is the stack switch itself or the realm's
#     idle Wait() serialising a send behind a receive.
#   - the counters that would answer that are unreachable in a shipping build:
#     netstat prints the `green:` line only inside the budget block, which is
#     AMINETXDUO_RXPROBE-gated, and an RXPROBE build is 4x slower to bring up.
#   - the test tier has never RUN against it.  It builds -- the whole tree,
#     tests included -- but tools/ci.sh's `green` arm is TESTS=OFF, so no
#     ctest and no emulator test has ever executed inside the realm.
option(AMINETXDUO_GREEN_REALM
       "Run the stack's ThreadX threads as green threads inside one Exec Task" OFF)
if(AMINETXDUO_GREEN_REALM)
    add_compile_definitions(AMINETXDUO_GREEN_REALM)
endif()

# Complete a waiting stream recv() directly on the IP thread.  The shipped
# design wakes the caller per receive notify and lets it re-enter the library
# to dequeue -- the budget's fetch leg, ~7.3 ms of wall behind the burst on
# the A1200.  ON has the blocking caller publish its buffer and park in a
# plain Wait(); the receive notify, already on the IP thread with the
# protection mutex held, dequeues, copies into that buffer and posts the one
# boundary Signal, so the caller returns without re-entering NetX and the
# window credit reopens at settle time instead of fetch time.  Anything but
# the plain blocking stream read (MSG_PEEK, MSG_WAITALL, OOB, non-blocking,
# finite timeouts) declines and takes the shipped path unchanged.
#
# WHY THE DEFAULT DID NOT MOVE.  A1200 at 68020 under Amiberry, bridged,
# ON and OFF alternating inside one block:
#
#   run-fitzbench.sh -k 512 -r 3, three rounds, read KB/s, ON vs OFF
#     a2065          492.0 vs 498.7   -1.3%
#     ariadne2       425.0 vs 430.7   -1.3%
#     ne2000_pcmcia  423.7 vs 446.7   -5.1%
#   tests/tools/run-iperf.sh tcp-rx (a plain BLOCKING recv, the shape this
#   fork serves), four rounds, bit/s, ON vs OFF: 4650914 vs 4680660, -0.6%,
#   inside the +-0.6% one-block spread and not the same sign in every round.
#
# The coverage counter says why, from an RXPROBE build of the same tcp-rx arm:
# `direct: 4 completed on the IP thread, 1125 classic dequeues`.  The fork is
# entered only on the FIRST iteration of the fill loop and only when the
# socket is empty at the call (transfer.c's `copied == 0` gate, and the pump
# declining in rxdirect.c when a packet is already queued).  A 68020 is the
# slow end of a bulk transfer, so the socket is nearly always non-empty when
# recv() is called and the buffer's remaining packets are classic by
# construction: it fires on 0.35% of dequeues and cannot move a bulk figure.
#
# WHAT WOULD MAKE IT WORTH REVISITING: a measurement on a request/response
# workload (tests/perf/run-reqresp.sh), where each recv() genuinely finds the
# socket empty, and a fork that also covers the `copied > 0` continuations.
# A default cannot move on that alone while the read figure is 1-5% down.
option(AMINETXDUO_RX_DIRECT_COMPLETE
       "Complete a waiting stream recv() directly on the IP thread" OFF)
if(AMINETXDUO_RX_DIRECT_COMPLETE)
    add_compile_definitions(AMINETXDUO_RX_DIRECT_COMPLETE)
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

# RFC 5827 early retransmit: below four segments in flight, three duplicate
# acknowledgments cannot happen, so the threshold drops to what the flight can
# reach.  No layout change, but a with/without measurement wants the whole tree
# built the same way, like the options above.
option(AMINETXDUO_TCP_EARLY_RETRANSMIT "Lower the fast retransmit threshold on a small flight, RFC 5827" ON)
if(NOT AMINETXDUO_TCP_EARLY_RETRANSMIT)
    add_compile_definitions(AMINETXDUO_TCP_EARLY_RETRANSMIT_OFF)
endif()

# RFC 8985 section 7.2 tail loss probe: one extra transmission two round trips
# after the last acknowledgment, instead of waiting out the timeout.  An
# NX_TCP_SOCKET layout change, so whole-tree.
option(AMINETXDUO_TCP_LOSS_PROBE "Probe for a lost tail segment before the timeout, RFC 8985" ON)
if(NOT AMINETXDUO_TCP_LOSS_PROBE)
    add_compile_definitions(AMINETXDUO_TCP_LOSS_PROBE_OFF)
endif()

# What ami_log_level() STARTS at: 0 = errors only ... 4 = trace.  Empty keeps
# compat.c's own default (WARN in release, DEBUG when AMINETXDUO_DEBUG is set).
# A running machine is turned up with ENV:ANXDLOGLEVEL, not with this.
set(AMINETXDUO_LOG_LEVEL "" CACHE STRING "Where ami_log() verbosity starts (0..4)")
if(NOT AMINETXDUO_LOG_LEVEL STREQUAL "")
    add_compile_definitions(AMINETXDUO_LOG_LEVEL=${AMINETXDUO_LOG_LEVEL})
    set(_aminetxduo_log_level_shown "${AMINETXDUO_LOG_LEVEL}")
elseif(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(_aminetxduo_log_level_shown "3 (debug)")
else()
    set(_aminetxduo_log_level_shown "1 (warn)")
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
    list(FILTER NETXDUO_SOURCES EXCLUDE REGEX "/(nx_icmpv6|nx_ipv6|nx_nd|nx_mld)_")
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

# The same mechanism again, for the source address selection: src/ipv6 supplies
# _nxd_ipv6_interface_find(), so the vendored object must not.  Unconditional
# rather than behind AMINETXDUO_IPV6, because the vendored file is #ifdef
# FEATURE_NX_IPV6 end to end and compiles to nothing in the floor build, where
# src/ipv6 is not added either.  There is no option to turn this off: the
# vendored routine is not a second implementation to compare against, it is
# the first-match walk docs/CONFORMANCE.md had a row for.
list(FILTER NETXDUO_SOURCES EXCLUDE REGEX "/nxd_ipv6_interface_find\\.c$")

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

# The DHCPv6 client, in the IPv6 build only.  It is the largest single addon
# here and every byte of it is unreachable without IPv6, so the floor build
# does not compile it -- the same arrangement src/netstack/CMakeLists.txt uses
# for netstack_ipv6.c.
if(AMINETXDUO_IPV6)
    list(APPEND NETXDUO_ADDON_SOURCES
        "${AMINETXDUO_NETXDUO}/addons/dhcp/nxd_dhcpv6_client.c")
endif()
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
# Wraps add_test() so that what CI builds is computed from what is registered
# instead of being kept in step by hand.  Must precede every add_test() call
# and every add_subdirectory() below.  cmake/HostTests.cmake says why.
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/HostTests.cmake")

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
    # The map is the only complete list of function entries: a hunk symbol
    # table holds no locals.  $<TARGET_FILE>.map is the path every target in
    # this tree that already asks for one uses, so a second ask is a no-op.
    target_link_options(${target} PRIVATE
        "-Wl,-Map=$<TARGET_FILE:${target}>.map")
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}"
                "-DBINARY=$<TARGET_FILE:${target}>"
                "-DMAPFILE=$<TARGET_FILE:${target}>.map"
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

# src/ipv6, on the same terms: it supplies _nxd_ipv6_interface_find(), which
# the vendored core now leaves undefined, so netxduo carries it as a PUBLIC
# dependency and every consumer picks it up without listing it.
if(AMINETXDUO_IPV6)
    add_subdirectory(src/ipv6)
    target_link_libraries(netxduo PUBLIC aminetxduo_ipv6)
endif()

add_subdirectory(src/common)
add_subdirectory(src/config)
add_subdirectory(src/sana2)
# The far side of src/sana2: anxnet.device is a SANA-II provider, not a
# consumer, and links none of the stack.
add_subdirectory(src/netdev)
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

# The remote-framebuffer frame encoder. Nothing links it yet; it is the shared
# half of the screen-streaming prototype, and it builds here so the m68k
# warning gate and the host round-trip cover it from the start.
add_subdirectory(src/rfb)

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

# Every add_test() in the tree has now run.  Write out the targets they need,
# for tools/ci.sh's host and sanitize stages to build.  Here and not earlier:
# a target defined in a directory added after the test that names it is still
# a target by this point, and was not one when the registration was recorded.
if(NOT CMAKE_CROSSCOMPILING)
    anx_write_host_test_targets("${CMAKE_BINARY_DIR}/host-test-targets.txt")
endif()

# ------------------------------------------------------------ the stamp ----
#
# aminetxduo_version_stamp rewrites <aminetxduo/version_git.h> with the commit
# actually being built (cmake/AmiNetXDuoVersion.cmake says why that cannot be
# done at configure time).  It has to finish before anything compiles, and
# every binary in the tree carries the string, so the dependency is added to
# every target rather than to the ones that remembered to ask -- same reason
# add_executable() is overridden above.  A target added next month gets it
# without anyone thinking about it.
#
# Only the header's CONTENT decides what recompiles: the stamp leaves the file
# untouched when the commit has not moved, so this edge costs a no-op build one
# git query and nothing else.
function(anx_stamp_all_targets dir)
    get_property(_targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(_target IN LISTS _targets)
        if(NOT _target STREQUAL "aminetxduo_version_stamp")
            add_dependencies(${_target} aminetxduo_version_stamp)
        endif()
    endforeach()
    get_property(_subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
    foreach(_subdir IN LISTS _subdirs)
        anx_stamp_all_targets("${_subdir}")
    endforeach()
endfunction()

anx_stamp_all_targets("${CMAKE_CURRENT_SOURCE_DIR}")

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
message(STATUS "  green realm ... ${AMINETXDUO_GREEN_REALM}")
message(STATUS "  mDNS .......... ${AMINETXDUO_MDNS}")
message(STATUS "  multicast ..... ${AMINETXDUO_MULTICAST}")
message(STATUS "  ARexx host .... ${AMINETXDUO_AREXX}")
message(STATUS "  serial log .... always; starts at ${_aminetxduo_log_level_shown}")
message(STATUS "  event ring .... ${AMINETXDUO_EVENT_RING} entries")
message(STATUS "  TCP: device ... ${AMINETXDUO_TCPDEVICE}")
