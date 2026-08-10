# Roadshow-era command set (docs/RESEARCH.md 3.3, 6.6).
#
# One AmigaOS executable per tool.  These are ordinary Shell commands, not
# library code, so newlib is available, but output goes through
# dos.library's Printf/VPrintf and arguments through ReadArgs(), which keeps
# them small and makes them behave like every other AmigaDOS command.
#
# Two flavours of tool:
#   * config-only  (AddNetInterface, Online, Offline, host), they never
#     touch NetX Duo directly, so they do not link ThreadX.
#   * stack-reading (ShowNetStatus, netstat, ping), they adopt themselves
#     as a TX_THREAD and read the live NX_IP, so they link the ThreadX port
#     and the SANA-II shim.

set(TOOLS_COMMON_SOURCES
    tool_util.c
    tool_diag.c
    netstack_weak.c)

# tool_devdiag.c is deliberately NOT in TOOLS_COMMON_SOURCES: it carries the
# SANA-II driver table and the card-diagnostic prose, which only the commands
# that configure an interface can reach.  See the head of that file for why
# --gc-sections cannot remove it from the ones that cannot.
set(TOOLS_DEVDIAG tool_devdiag.c)


set(TOOLS_WARNINGS -Wall -Wextra)

# The commands are built for size, the libraries for speed.
#
# Nothing here is a hot path: ping waits on the wire, ShowNetStatus prints a
# table once, and there is no throughput number the level can move, the
# libraries that actually move bytes are compiled separately.
#
# Re-measured 2026-07-27 over all 21 commands, and it does NOT reproduce the
# "14-17% against -O2" this comment used to claim.  Against -O3 the saving is
# 9.0% (774,096 vs 850,968 bytes total, -3.4% on host to -15.8% on NetTrace);
# against -O2 it is 4.3%.  Two caveats before treating either number as the
# last word: the old figure says it was taken on the PINNED toolchain and this
# one was taken on the local one, which ships a different crt0 and may differ
# elsewhere; and -ffunction-sections with --gc-sections has been added since,
# which already removes much of what -Os used to save.  Both numbers may be
# right about the tree they were taken on.
#
# Appended after CMAKE_C_FLAGS_RELEASE so it wins the duplicate -O.  There are
# two duplicates to win, in fact: CMake's own GNU defaults append "-O3 -DNDEBUG"
# after whatever the toolchain file puts in CMAKE_C_FLAGS_RELEASE_INIT, so the
# libraries are -O3 builds and not the -O2 that file asks for.  See
# docs/RESEARCH.md §21.6, it is a real finding and deliberately left alone.
#
# AMINETXDUO_TOOLS_OPTIMISE makes the level measurable instead of a constant
# somebody has to edit.  Note it governs a command's OWN objects only: a tool
# that asks for NX links aminetxduo_sana2, aminetxduo_config, threadx_port and
# netxduo_port, which are built at the tree's level, so a command is a MIX,
# and its size moves when the global level moves even with this pinned.  That
# is measured, not assumed: ping is 34,652 bytes in an -O2 tree and 35,572 in
# an -O3 one with -Os set here in both.
set(AMINETXDUO_TOOLS_OPTIMISE "-Os" CACHE STRING
    "Optimisation level for the commands' own objects")
set(TOOLS_OPTIMISE $<$<CONFIG:Release>:${AMINETXDUO_TOOLS_OPTIMISE}>)

# The libnix crt0.o register-save bug that made every command crash on return
# to the Shell is repaired in the TOOLCHAIN, by tools/fix-toolchain-crt0.py at
# fetch time, not here.  It was briefly worked around per target, by patching
# a private crt0 and linking it first under -nostartfiles; that had to be
# repeated for every command, had to be right once per CPU multilib (there are
# four affected crt0.o files), and left the toolchain broken for anything not
# built through this file.  Nothing about it belongs in a link line.
#
# `tools/fetch-toolchain.sh --check-crt0` says whether the toolchain in use has
# been repaired, or does not need repairing.  Reported as bebbo/amiga-gcc
# issue #12 and fixed there: m68k_save_reg() now honours the __entrypoint
# attribute crt0.c already carried, on amiga15.2 (168be3619), amiga13.4 and
# amiga16.1.  A toolchain built from those reports "immune".  The repair stays for the toolchains
# that predate it; nothing in it runs against one that does not need it.

# Everything the tools include lives behind the core include set; the port
# headers must stay first (tx_port.h / nx_port.h / nx_user.h).
function(aminetxduo_add_tool target output)
    cmake_parse_arguments(TOOL "NX" "" "SOURCES;DEFINES" ${ARGN})

    add_executable(${target} ${TOOL_SOURCES} ${TOOLS_COMMON_SOURCES})

    # src/sana2 is on the path for its device header alone: the NDK ships no
    # <devices/sana2.h>, and tool_diag.c opens a SANA-II device to find out
    # whether the user's card really is there.  No tool links the shim unless
    # it asks for NX.
    target_include_directories(${target} PRIVATE
        ${AMINETXDUO_CORE_INCLUDES}
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_SOURCE_DIR}/src/sana2")

    if(AMIGA_NDK_INCLUDE)
        target_include_directories(${target} PRIVATE "${AMIGA_NDK_INCLUDE}")
    endif()

    target_compile_options(${target} PRIVATE ${TOOLS_WARNINGS} ${TOOLS_OPTIMISE})

    # A command is an ordinary executable with one entry point, so everything
    # unreachable from main() is dead and the linker may be told to say so.
    # This is where the size of these commands actually was, NOT newlib,
    # which contributes 4 KB of the 37 KB Online used to be.
    # TOOLS_COMMON_SOURCES is named in add_executable() rather than pulled
    # from an archive, so before --gc-sections the linker had no choice but to
    # keep every byte of tool_diag.c in every command, whether that command
    # ever explains anything or not.  Measured: -27% on Online, -38% on host.
    # See docs/RESEARCH.md §21.
    #
    # --gc-sections is deliberately confined to this directory: the .library
    # targets are reached through a romtag that Exec finds by scanning the
    # loaded segment, with no relocation pointing at it, so collecting one
    # throws the library away (measured: 60 KB off tls.library).
    # -fno-optimize-sibling-calls is not a tuning choice; see the block above
    # and docs/RESEARCH.md §25.  It travels with -ffunction-sections.
    target_compile_options(${target} PRIVATE
        -ffunction-sections -fno-optimize-sibling-calls)
    target_link_options(${target} PRIVATE -Wl,--gc-sections)

    # A map per tool, as the profiler targets already get.  Without one a
    # profile of a tool resolves no symbols and ranks the tool by module, and
    # a module that is one small library object then reads as a hot function.
    # profreport refuses that case outright, so the map is what makes a tool
    # profilable at all.
    target_link_options(${target} PRIVATE
        "-Wl,-Map=$<TARGET_FILE_DIR:${target}>/${output}.map")

    if(TOOL_DEFINES)
        target_compile_definitions(${target} PRIVATE ${TOOL_DEFINES})
    endif()

    if(TOOL_NX)
        # tool_nx.c adopts the calling Process as a TX_THREAD and reads the
        # SANA-II counters out of nx_interface_additional_link_info.
        target_sources(${target} PRIVATE tool_nx.c)
        target_link_libraries(${target} PRIVATE
            aminetxduo_sana2 aminetxduo_config threadx_port netxduo_port)
    else()
        target_link_libraries(${target} PRIVATE aminetxduo_config)
    endif()

    set_target_properties(${target} PROPERTIES OUTPUT_NAME "${output}")

    # The one thing --gc-sections can quietly take that no test would notice:
    # the "$VER:" tag AmigaDOS finds by scanning the file rather than by any
    # reference the linker can see.  Checked on the linked binary, every time.
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}"
                "-DTOOL_BINARY=$<TARGET_FILE:${target}>"
                -P "${CMAKE_SOURCE_DIR}/cmake/check-version-tag.cmake"
        VERBATIM)

    # Symbols, after the version tag is checked and before the size is printed,
    # so the number reported is the number that ships.  A command carries its
    # symbol table for nobody: the profiler reads the linker map and the object
    # files rather than HUNK_SYMBOL (tools/profiler/profreport.py), and a
    # crash on this machine is diagnosed from an address and the map, not from
    # the binary.  Worth about 4 KB on ping, which is 15% of it.
    if(CMAKE_STRIP AND NOT AMINETXDUO_KEEP_SYMBOLS)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "${CMAKE_STRIP}" "$<TARGET_FILE:${target}>"
            VERBATIM)
    endif()

    if(AMIGA_SIZE)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "${AMIGA_SIZE}" "$<TARGET_FILE:${target}>"
            COMMENT "${output} size")
    endif()
endfunction()

aminetxduo_add_tool(tool_addnetinterface AddNetInterface
    SOURCES addnetinterface.c ${TOOLS_DEVDIAG})

# CheckNetConfig is config-only in the strictest sense there is: it never opens
# bsdsocket.library at all.  That is not an omission, the machine that needs
# checking is the one where the stack did not come up, so a checker that needed
# a running stack would be unavailable in precisely the state it is for.
aminetxduo_add_tool(tool_checknetconfig CheckNetConfig
    SOURCES checknetconfig.c ${TOOLS_DEVDIAG})

# The interactive configurator.  Config-only: it writes the files the parser
# reads and then runs AddNetInterface, rather than starting a stack itself.
aminetxduo_add_tool(tool_netsetup NetSetup SOURCES netsetup.c ${TOOLS_DEVDIAG})

# AddNetRoute and DeleteNetRoute are one source, the same way Online and
# Offline are: they parse the same destination and differ in one library call.
# Config-only, they reach the live routing table through NetStackControl()
# and never link NetX Duo.
aminetxduo_add_tool(tool_addnetroute    AddNetRoute    SOURCES addnetroute.c)
aminetxduo_add_tool(tool_deletenetroute DeleteNetRoute SOURCES addnetroute.c
    DEFINES TOOL_DELETE)

# GetNetStatus is the scriptable counterpart of ShowNetStatus: it answers with
# a return code rather than a table.  NX, because it answers from the SAME two
# snapshots ShowNetStatus and netstat print from, three commands that disagree
# about whether the network is up would be worse than not having the third.
aminetxduo_add_tool(tool_getnetstatus GetNetStatus NX SOURCES getnetstatus.c)

# NetShutdown stops every interface at once.  Config-only: it reaches the live
# interfaces through NetStackControl(), exactly as Offline does.
aminetxduo_add_tool(tool_netshutdown NetShutdown SOURCES netshutdown.c)

# RemoveNetInterface takes one interface out, where NetShutdown stops them all
# and keeps them.  Config-only for the same reason: NetStackControl() again.
aminetxduo_add_tool(tool_removenetinterface RemoveNetInterface
    SOURCES removenetinterface.c)

aminetxduo_add_tool(tool_online  Online   SOURCES onoff.c ${TOOLS_DEVDIAG})
aminetxduo_add_tool(tool_offline Offline  SOURCES onoff.c ${TOOLS_DEVDIAG}
    DEFINES TOOL_OFFLINE)

# host resolves through bsdsocket.library like every other client command,
# and toolsock.c is where tool_socket_open() lives, opening the library is
# what starts the stack, which host used to refuse to do.
aminetxduo_add_tool(tool_host    host     SOURCES host.c toolsock.c)

# nslookup asks for the record types host cannot: MX, TXT, NS, SRV, SOA and
# CNAME.  It is config-only in the same sense as tftp, one datagram socket
# through the published vectors, and toolsock.c for them, because there is no
# published call that asks for an MX and it therefore writes the RFC 1035 query
# itself.  See the header of nslookup.c for why that is the right shape.
aminetxduo_add_tool(tool_nslookup nslookup SOURCES nslookup.c toolsock.c)

# arp reads NETSTATUS_ARP and writes through NETCTRL_ARP_ADD/DELETE, both
# already existed with nothing driving them.  Status-only, so it must never
# start the stack: see the header of arp.c.
aminetxduo_add_tool(tool_arp     arp      NX SOURCES arp.c)

# fetch is config-only in the same sense: it never touches NetX Duo directly.
# It reaches bsdsocket.library and tls.library through their published vectors
# at run time, so ONE binary serves both -DAMINETXDUO_TLS=ON and OFF builds --
# the https: path opens LIBS:tls.library and says something legible when there
# is none.  Nothing here is conditional on the build option, deliberately.
aminetxduo_add_tool(tool_fetch   fetch    SOURCES fetch.c fetchurl.c toolsock.c)

# nc and telnet are config-only in the same sense as fetch, and share
# toolsock.c: bsdsocket.library through its published vectors, including the
# bind/listen/accept half that no client-only command reaches.  Neither needs
# TLS, and neither needs a libc porting shim, which is what makes them
# writable natively rather than ported.
aminetxduo_add_tool(tool_nc      nc       SOURCES nc.c     toolsock.c)

# NetTrace is config-only in exactly the same sense, and deliberately so: it
# reaches the capture path through the eight published bpf_* LVOs rather than
# by linking src/bpf/, because a tool that linked the archive would get its
# OWN copy of the channel table and capture nothing at all.
aminetxduo_add_tool(tool_nettrace NetTrace SOURCES nettrace.c toolsock.c)
aminetxduo_add_tool(tool_telnet  telnet   SOURCES telnet.c toolsock.c)

# traceroute is config-only too, and is the first command here to open a
# SOCK_RAW descriptor, so it reaches the raw half of the ABI the same way it
# reaches every other part of it, through the published vectors.
aminetxduo_add_tool(tool_traceroute traceroute SOURCES traceroute.c toolsock.c)

# tftp and whois are clients and nothing more, so they are config-only in the
# plainest sense: one datagram socket and one stream socket respectively,
# through the published vectors, and no part of the stack linked in.
aminetxduo_add_tool(tool_tftp    tftp     SOURCES tftp.c  toolsock.c)
aminetxduo_add_tool(tool_whois   whois    SOURCES whois.c toolsock.c)

# CensusProbe is the guest half of the allocation census, and exists only when
# the census does.  It is not a command anybody installs: it walks the sites an
# ordinary command walks and then provokes the library expunge, which is the
# only moment at which "nothing is outstanding" is a meaningful claim and which
# nothing else in the tree reaches.  src/tools/censusprobe.c says why.
if(AMINETXDUO_ALLOCCENSUS)
    aminetxduo_add_tool(tool_censusprobe CensusProbe
        SOURCES censusprobe.c toolsock.c)
endif()

# httpd is the first SERVER here, and config-only in the same sense as the
# clients: bsdsocket.library through its published vectors, and no part of the
# stack linked in.  It is the only thing in the tree that holds several
# connections open at once, `nc -l` takes one caller and exits, so it is
# also the only coverage of accept() being called twice on a live listener.
#
# httppath.c, httpif.c, httpframe.c and httplock.c are sources of their own
# rather than part of httpd.c because they are the parts that must not be
# wrong, and
# splitting them is what lets the host tests below compile them: httpd.c
# includes tools.h, which reaches tx_api.h and proto/dos.h, and neither builds
# on a host.  One decides which file a request reaches, the second whether it
# may write to it, the third where one request ends and the next begins, the
# fourth who may write to a name somebody else has locked.
# iperfcore.c and iperfwire.c are the same measurement the `iperf` command
# runs, driven from httpd's own loop a slice at a time so that a user with a
# browser and no Shell can produce a figure.  See the iperf block in httpd.c.
aminetxduo_add_tool(tool_httpd   httpd    SOURCES httpd.c httppath.c httpif.c
                                          httpframe.c httplock.c toolsock.c
                                          iperfcore.c iperfwire.c)

# iperf measures throughput, and is config-only in the same sense as the other
# clients here: bsdsocket.library through its published vectors and no part of
# the stack linked in.  That is the point rather than a convenience -- the
# figure it prints is the one an application gets, and the same binary runs on
# Roadshow and AmiTCP, so a user who says we are slower than another stack can
# measure both with one tool on their own card.
#
# It does NOT link third_party/netxduo/utility/iperf/nx_iperf.c, for the reason
# sntp.c gives about the vendored SNTP add-on and two more: that file will not
# compile without nx_web_http_server.h and FileX, which this tree does not
# ship, and it binds its sockets on an NX_IP a Shell command's own linked copy
# of NetX Duo does not have.  iperfwire.c reimplements the wire format it
# speaks, which was read off iperf 2.2.1 rather than out of that source.
#
# iperfwire.c is a source of its own for the same reason httppath.c is: it is
# the part that must not be wrong, and keeping it free of AmigaDOS lets the
# host test below compile it.
aminetxduo_add_tool(tool_iperf   iperf    SOURCES iperf.c iperfcore.c
                                          iperfwire.c toolsock.c)

# sntp is config-only in the same sense as fetch: it reaches bsdsocket.library
# through its published vectors and sets the machine's clock through
# timer.device and battclock.resource.  It does NOT link the vendored NetX Duo
# SNTP add-on, and the comment at the top of sntp.c says why, the short
# version is that the add-on wants a running ThreadX, and a Shell command's
# copy of ThreadX is not the one the stack is using.
aminetxduo_add_tool(tool_sntp    sntp     SOURCES sntp.c toolsock.c)

# ShowNetServices browses for mDNS services.  Config-only: it starts and stops
# the browse through NETCTRL_MDNS_BROWSE and reads the answers through
# NETSTATUS_SERVICES, and never touches an NX_IP, the collection window is a
# Delay() on its own task, which is the whole reason it does not need one.
aminetxduo_add_tool(tool_shownetservices ShowNetServices
    SOURCES shownetservices.c)

aminetxduo_add_tool(tool_shownetstatus ShowNetStatus NX
    SOURCES shownetstatus.c ${TOOLS_DEVDIAG})
aminetxduo_add_tool(tool_netstat       netstat       NX SOURCES netstat.c)
# ping is config-only now, and that is the fix rather than a tidy-up: it used
# to link its own ThreadX and NetX Duo and call nx_icmp_ping() on an NX_IP that
# no shipped build ever had.  It sends its own ICMP echo over a SOCK_RAW
# descriptor through the published vectors, exactly as traceroute does.
aminetxduo_add_tool(tool_ping          ping          SOURCES ping.c toolsock.c)

# ---------------------------------------------------------------- smoke ---
#
# Driver for tools/fsuae-run.sh, which runs exactly one executable with no
# arguments.  ToolsSmoke runs the real tools through SystemTagList() and
# leaves their output in DH0:tools.txt, which the harness prints back.
#
#   cmake --build build/cm --parallel --target tools_smoke
#   ./tools/fsuae-run.sh -t 120 build/cm/src/tools/ToolsSmoke \
#       build/cm/src/tools/AddNetInterface build/cm/src/tools/Online \
#       build/cm/src/tools/Offline build/cm/src/tools/ShowNetStatus \
#       build/cm/src/tools/ping build/cm/src/tools/netstat \
#       build/cm/src/tools/host src/tools/testdata/Devs

add_executable(tool_smoke toolssmoke.c)
target_include_directories(tool_smoke PRIVATE ${AMINETXDUO_CORE_INCLUDES})
if(AMIGA_NDK_INCLUDE)
    target_include_directories(tool_smoke PRIVATE "${AMIGA_NDK_INCLUDE}")
endif()
target_compile_options(tool_smoke PRIVATE ${TOOLS_WARNINGS} ${TOOLS_OPTIMISE})
set_target_properties(tool_smoke PROPERTIES OUTPUT_NAME "ToolsSmoke")

# ------------------------------------------------------- the LVO stubs ---
#
# toolsock.c enters bsdsocket.library with its arguments in local register
# variables, and a call made after one of them is set clobbers the earlier
# ones.  Compiled here at -O0 and disassembled, because -Os inlines the only
# helper involved and hides it, see cmake/check-lvo-stub-registers.cmake.
#
# A target of its own rather than a POST_BUILD on a command: the check wants
# the file at an optimisation level no command is built at, and running it once
# is enough for the nine commands that share the file.
if(CMAKE_CROSSCOMPILING)
    add_custom_target(tool_lvo_stub_check ALL
        COMMAND "${CMAKE_COMMAND}"
                "-DCC=${CMAKE_C_COMPILER}"
                "-DCFLAGS=${CMAKE_C_FLAGS} -I$<JOIN:$<TARGET_PROPERTY:tool_ping,INCLUDE_DIRECTORIES>, -I>"
                "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/toolsock.c"
                "-DOBJDUMP=${CMAKE_OBJDUMP}"
                "-DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}/lvo-check"
                -P "${CMAKE_SOURCE_DIR}/cmake/check-lvo-stub-registers.cmake"
        COMMENT "toolsock.c enters the library with its own arguments"
        VERBATIM)
endif()

add_custom_target(tools ALL)
add_dependencies(tools
    tool_addnetinterface tool_checknetconfig
    tool_addnetroute tool_deletenetroute
    tool_getnetstatus tool_netshutdown tool_removenetinterface
    tool_netsetup tool_online tool_offline tool_host
    tool_fetch tool_nc tool_telnet tool_nettrace tool_traceroute tool_nslookup tool_arp
    tool_tftp tool_whois tool_httpd tool_iperf
    tool_sntp
    tool_shownetstatus tool_netstat tool_ping tool_smoke)
if(CMAKE_CROSSCOMPILING)
    add_dependencies(tools tool_lvo_stub_check)
endif()

# --------------------------------------------------------------- host tests --
#
# Host only: httppath.c, httpif.c and fetchurl.c are the files here free of
# AmigaDOS calls, and the ones whose mistakes a reviewer cannot be relied on to
# see.  One decides which file a request reaches, the second whether the
# request may have it, the third where a redirect goes, and an If: evaluator
# that always says yes reads exactly like one that works, as does a resolver
# that turns "about.html" into a host name.
#
#   cc -std=c11 -Wall -Wextra -Isrc/tools \
#      src/tools/test/test_httppath.c src/tools/httppath.c -o test_httppath
#   cc -std=c11 -Wall -Wextra -Isrc/tools \
#      src/tools/test/test_httpif.c src/tools/httpif.c -o test_httpif
#   cc -std=c11 -Wall -Wextra -Isrc/tools \
#      src/tools/test/test_fetchurl.c src/tools/fetchurl.c -o test_fetchurl
#
# No target_link_libraries: every library target here reaches AmigaOS code
# that does not build on a host, which is why the sources are named directly
# (src/mbuf/CMakeLists.txt and tests/fuzz/CMakeLists.txt say the same).
if(NOT CMAKE_CROSSCOMPILING)
    add_executable(test_httppath
        test/test_httppath.c
        httppath.c)

    target_include_directories(test_httppath PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}")

    target_compile_options(test_httppath PRIVATE -Wall -Wextra)
    set_target_properties(test_httppath PROPERTIES C_STANDARD 11)

    add_executable(test_httpif
        test/test_httpif.c
        httpif.c)

    target_include_directories(test_httpif PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}")

    target_compile_options(test_httpif PRIVATE -Wall -Wextra)
    set_target_properties(test_httpif PROPERTIES C_STANDARD 11)

    # The ReadArgs templates of every command, against the enums that index
    # them.  Parses the sources rather than compiling them: they reach
    # proto/dos.h, and the failure it catches is a template and an enum drifting
    # apart, which no compiler sees.
    add_executable(test_argtemplates
        test/test_argtemplates.c)

    target_compile_definitions(test_argtemplates PRIVATE
        AMINETXDUO_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
        _GNU_SOURCE)

    target_compile_options(test_argtemplates PRIVATE -Wall -Wextra)
    set_target_properties(test_argtemplates PROPERTIES C_STANDARD 11)

    add_executable(test_httplock
        test/test_httplock.c
        httplock.c
        httppath.c)

    target_include_directories(test_httplock PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}")

    target_compile_options(test_httplock PRIVATE -Wall -Wextra)
    set_target_properties(test_httplock PROPERTIES C_STANDARD 11)

    add_executable(test_fetchurl
        test/test_fetchurl.c
        fetchurl.c)

    target_include_directories(test_fetchurl PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}")

    target_compile_options(test_fetchurl PRIVATE -Wall -Wextra)
    set_target_properties(test_fetchurl PROPERTIES C_STANDARD 11)

    # The iperf 2 wire format.  Host-side because that is where the numbers
    # captured off iperf 2.2.1 can be asserted against the encoder, and
    # because unsigned long is 64 bits here and 32 on the target: the sign of
    # a datagram id and the carry out of the byte counter were both wrong
    # until this compiled them the other way.
    add_executable(test_iperfwire
        test/test_iperfwire.c
        iperfwire.c)

    target_include_directories(test_iperfwire PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}")

    target_compile_options(test_iperfwire PRIVATE -Wall -Wextra)
    set_target_properties(test_iperfwire PROPERTIES C_STANDARD 11)

    add_executable(test_httpframe
        test/test_httpframe.c
        httpframe.c)

    target_include_directories(test_httpframe PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}")

    target_compile_options(test_httpframe PRIVATE -Wall -Wextra)
    set_target_properties(test_httpframe PROPERTIES C_STANDARD 11)

    include(CTest)
    add_test(NAME httppath_escapes COMMAND test_httppath)
    add_test(NAME httpif_conditions COMMAND test_httpif)
    add_test(NAME fetchurl_rfc3986 COMMAND test_fetchurl)
    add_test(NAME httpframe_bodies COMMAND test_httpframe)
    add_test(NAME httplock_rfc4918 COMMAND test_httplock)
    add_test(NAME iperf_wire_format COMMAND test_iperfwire)
    add_test(NAME tools_arg_templates COMMAND test_argtemplates)
endif()
