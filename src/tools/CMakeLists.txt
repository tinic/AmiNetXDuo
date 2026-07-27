# Roadshow-era command set (docs/RESEARCH.md 3.3, 6.6).
#
# One AmigaOS executable per tool.  These are ordinary Shell commands, not
# library code, so newlib is available -- but output goes through
# dos.library's Printf/VPrintf and arguments through ReadArgs(), which keeps
# them small and makes them behave like every other AmigaDOS command.
#
# Two flavours of tool:
#   * config-only  (AddNetInterface, Online, Offline, host) -- they never
#     touch NetX Duo directly, so they do not link ThreadX.
#   * stack-reading (ShowNetStatus, netstat, ping) -- they adopt themselves
#     as a TX_THREAD and read the live NX_IP, so they link the ThreadX port
#     and the SANA-II shim.

set(TOOLS_COMMON_SOURCES
    tool_util.c
    tool_diag.c
    netstack_weak.c)

set(TOOLS_WARNINGS -Wall -Wextra)

# The commands are built for size, the libraries for speed.
#
# Nothing here is a hot path: ping waits on the wire, ShowNetStatus prints a
# table once.  -Os buys 14-17% off every command against -O2, measured on the
# pinned toolchain, and there is no throughput number it can move -- the
# libraries that actually move bytes are compiled separately.
#
# Appended after CMAKE_C_FLAGS_RELEASE so it wins the duplicate -O.  There are
# two duplicates to win, in fact: CMake's own GNU defaults append "-O3 -DNDEBUG"
# after whatever the toolchain file puts in CMAKE_C_FLAGS_RELEASE_INIT, so the
# libraries are -O3 builds and not the -O2 that file asks for.  See
# docs/RESEARCH.md §21.6 -- it is a real finding and deliberately left alone.
set(TOOLS_OPTIMISE $<$<CONFIG:Release>:-Os>)

# ------------------------------------------------------------------ crt0 -----
#
# The toolchain's libnix crt0.o saves d2/a2/a6 at _start but restores
# d2/d7/a2/a6 at _exit (see clients/compat/fix-crt0.py).  The extra register on
# the restore shifts the frame one longword: the rts returns a command into the
# stack-size word RunCommand pushes above its return address instead of the
# return address, so EVERY command crashes the instant it returns to the Shell.
# It is invisible under emulator smoke tests because those never run a compiled
# command to completion under a real Shell -- it only bit once these ran on a
# real A3000.  (The .library targets are unaffected: a shared library has no
# crt0.)  fix-crt0.py patches a private copy (2022 -> 2122, adding d7 to the
# _start save); we link it as the FIRST object under -nostartfiles so _start
# stays at offset 0 of the first hunk, which is where LoadSeg() enters.
find_package(Python3 COMPONENTS Interpreter REQUIRED)

separate_arguments(TOOLS_ARCH_LIST UNIX_COMMAND "${AMIGA_ARCH_FLAGS}")
execute_process(
    COMMAND "${CMAKE_C_COMPILER}" ${TOOLS_ARCH_LIST} -print-file-name=crt0.o
    OUTPUT_VARIABLE TOOLS_STOCK_CRT0
    OUTPUT_STRIP_TRAILING_WHITESPACE)

set(TOOLS_CRT0 "${CMAKE_CURRENT_BINARY_DIR}/crt0-fixed.o")
add_custom_command(
    OUTPUT "${TOOLS_CRT0}"
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_SOURCE_DIR}/clients/compat/fix-crt0.py"
            "${TOOLS_STOCK_CRT0}" "${TOOLS_CRT0}"
    DEPENDS "${CMAKE_SOURCE_DIR}/clients/compat/fix-crt0.py" "${TOOLS_STOCK_CRT0}"
    COMMENT "Patching libnix crt0.o register-save bug (${TOOLS_STOCK_CRT0})"
    VERBATIM)
add_custom_target(tools_crt0 DEPENDS "${TOOLS_CRT0}")
set_source_files_properties("${TOOLS_CRT0}"
    PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)

# Everything the tools include lives behind the core include set; the port
# headers must stay first (tx_port.h / nx_port.h / nx_user.h).
function(aminetxduo_add_tool target output)
    cmake_parse_arguments(TOOL "NX" "" "SOURCES;DEFINES" ${ARGN})

    # The patched crt0 is the FIRST object so _start lands at offset 0; the
    # matching -nostartfiles is added with the other link options below.
    add_executable(${target} "${TOOLS_CRT0}" ${TOOL_SOURCES} ${TOOLS_COMMON_SOURCES})
    add_dependencies(${target} tools_crt0)

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
    # This is where the size of these commands actually was -- NOT newlib,
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
    # -nostartfiles: we supply the repaired crt0 as the first object above, so
    # the toolchain's buggy stock one must not also be pulled in.
    target_link_options(${target} PRIVATE -nostartfiles -Wl,--gc-sections)

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

    aminetxduo_check_pcrel(${target})

    if(AMIGA_SIZE)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "${AMIGA_SIZE}" "$<TARGET_FILE:${target}>"
            COMMENT "${output} size")
    endif()
endfunction()

aminetxduo_add_tool(tool_addnetinterface AddNetInterface
    SOURCES addnetinterface.c)

# CheckNetConfig is config-only in the strictest sense there is: it never opens
# bsdsocket.library at all.  That is not an omission -- the machine that needs
# checking is the one where the stack did not come up, so a checker that needed
# a running stack would be unavailable in precisely the state it is for.
aminetxduo_add_tool(tool_checknetconfig CheckNetConfig
    SOURCES checknetconfig.c)

# The interactive configurator.  Config-only: it writes the files the parser
# reads and then runs AddNetInterface, rather than starting a stack itself.
aminetxduo_add_tool(tool_netsetup NetSetup SOURCES netsetup.c)

# AddNetRoute and DeleteNetRoute are one source, the same way Online and
# Offline are: they parse the same destination and differ in one library call.
# Config-only -- they reach the live routing table through NetStackControl()
# and never link NetX Duo.
aminetxduo_add_tool(tool_addnetroute    AddNetRoute    SOURCES addnetroute.c)
aminetxduo_add_tool(tool_deletenetroute DeleteNetRoute SOURCES addnetroute.c
    DEFINES TOOL_DELETE)

# GetNetStatus is the scriptable counterpart of ShowNetStatus: it answers with
# a return code rather than a table.  NX, because it answers from the SAME two
# snapshots ShowNetStatus and netstat print from -- three commands that disagree
# about whether the network is up would be worse than not having the third.
aminetxduo_add_tool(tool_getnetstatus GetNetStatus NX SOURCES getnetstatus.c)

# NetShutdown stops every interface at once.  Config-only: it reaches the live
# interfaces through NetStackControl(), exactly as Offline does.
aminetxduo_add_tool(tool_netshutdown NetShutdown SOURCES netshutdown.c)

aminetxduo_add_tool(tool_online  Online   SOURCES onoff.c)
aminetxduo_add_tool(tool_offline Offline  SOURCES onoff.c DEFINES TOOL_OFFLINE)

aminetxduo_add_tool(tool_host    host     SOURCES host.c)

# fetch is config-only in the same sense: it never touches NetX Duo directly.
# It reaches bsdsocket.library and tls.library through their published vectors
# at run time, so ONE binary serves both -DAMINETXDUO_TLS=ON and OFF builds --
# the https: path opens LIBS:tls.library and says something legible when there
# is none.  Nothing here is conditional on the build option, deliberately.
aminetxduo_add_tool(tool_fetch   fetch    SOURCES fetch.c)

# nc, telnet and ftp are config-only in the same sense as fetch, and share
# toolsock.c: bsdsocket.library through its published vectors, including the
# bind/listen/accept half that no client-only command reaches.  None of them
# needs TLS, and none of them needs a libc porting shim -- which is what makes
# them the three that can be written natively rather than ported.
aminetxduo_add_tool(tool_nc      nc       SOURCES nc.c     toolsock.c)

# NetTrace is config-only in exactly the same sense, and deliberately so: it
# reaches the capture path through the eight published bpf_* LVOs rather than
# by linking src/bpf/, because a tool that linked the archive would get its
# OWN copy of the channel table and capture nothing at all.
aminetxduo_add_tool(tool_nettrace NetTrace SOURCES nettrace.c toolsock.c)
aminetxduo_add_tool(tool_telnet  telnet   SOURCES telnet.c toolsock.c)
aminetxduo_add_tool(tool_ftp     ftp      SOURCES ftp.c    toolsock.c)

# traceroute is config-only too, and is the first command here to open a
# SOCK_RAW descriptor -- so it reaches the raw half of the ABI the same way it
# reaches every other part of it, through the published vectors.
aminetxduo_add_tool(tool_traceroute traceroute SOURCES traceroute.c toolsock.c)

# tftp and whois are clients and nothing more, so they are config-only in the
# plainest sense: one datagram socket and one stream socket respectively,
# through the published vectors, and no part of the stack linked in.
aminetxduo_add_tool(tool_tftp    tftp     SOURCES tftp.c  toolsock.c)
aminetxduo_add_tool(tool_whois   whois    SOURCES whois.c toolsock.c)

# sntp is config-only in the same sense as fetch: it reaches bsdsocket.library
# through its published vectors and sets the machine's clock through
# timer.device and battclock.resource.  It does NOT link the vendored NetX Duo
# SNTP add-on, and the comment at the top of sntp.c says why -- the short
# version is that the add-on wants a running ThreadX, and a Shell command's
# copy of ThreadX is not the one the stack is using.
aminetxduo_add_tool(tool_sntp    sntp     SOURCES sntp.c)

aminetxduo_add_tool(tool_shownetstatus ShowNetStatus NX
    SOURCES shownetstatus.c)
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

add_custom_target(tools ALL)
add_dependencies(tools
    tool_addnetinterface tool_checknetconfig
    tool_addnetroute tool_deletenetroute
    tool_getnetstatus tool_netshutdown
    tool_netsetup tool_online tool_offline tool_host
    tool_fetch tool_nc tool_telnet tool_ftp tool_nettrace tool_traceroute
    tool_tftp tool_whois
    tool_sntp
    tool_shownetstatus tool_netstat tool_ping tool_smoke)
