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

    target_compile_options(${target} PRIVATE ${TOOLS_WARNINGS})

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

    if(AMIGA_SIZE)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "${AMIGA_SIZE}" "$<TARGET_FILE:${target}>"
            COMMENT "${output} size")
    endif()
endfunction()

aminetxduo_add_tool(tool_addnetinterface AddNetInterface
    SOURCES addnetinterface.c)

# The interactive configurator.  Config-only: it writes the files the parser
# reads and then runs AddNetInterface, rather than starting a stack itself.
aminetxduo_add_tool(tool_netsetup NetSetup SOURCES netsetup.c)

aminetxduo_add_tool(tool_online  Online   SOURCES onoff.c)
aminetxduo_add_tool(tool_offline Offline  SOURCES onoff.c DEFINES TOOL_OFFLINE)

aminetxduo_add_tool(tool_host    host     SOURCES host.c)

# fetch is config-only in the same sense: it never touches NetX Duo directly.
# It reaches bsdsocket.library and tls.library through their published vectors
# at run time, so ONE binary serves both -DAMINETXDUO_TLS=ON and OFF builds --
# the https: path opens LIBS:tls.library and says something legible when there
# is none.  Nothing here is conditional on the build option, deliberately.
aminetxduo_add_tool(tool_fetch   fetch    SOURCES fetch.c)

aminetxduo_add_tool(tool_shownetstatus ShowNetStatus NX
    SOURCES shownetstatus.c)
aminetxduo_add_tool(tool_netstat       netstat       NX SOURCES netstat.c)
aminetxduo_add_tool(tool_ping          ping          NX SOURCES ping.c)

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
target_compile_options(tool_smoke PRIVATE ${TOOLS_WARNINGS})
set_target_properties(tool_smoke PROPERTIES OUTPUT_NAME "ToolsSmoke")

add_custom_target(tools ALL)
add_dependencies(tools
    tool_addnetinterface tool_netsetup tool_online tool_offline tool_host
    tool_fetch tool_shownetstatus tool_netstat tool_ping tool_smoke)
