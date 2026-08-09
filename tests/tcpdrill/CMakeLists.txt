# tcpdrill, packet-level TCP conformance.
#
# One AmigaOS executable holding both halves of the experiment: a synthetic
# SANA-II device that the shipped stack opens exactly as it opens
# a2065.device, and a script engine that drives socket calls against
# bsdsocket.library while asserting on every frame the stack emits.
#
# It links NOTHING from src/.  Every socket call is a published LVO and every
# packet is seen through the device interface, so a change that broke the
# library's ABI or its SANA-II contract would fail here rather than be
# compiled around.  src/sana2/sana2_device.h is included for the SANA-II
# command numbers and structures alone, they are protocol facts, and the
# alternative is a second copy of them.

add_executable(test_tcpdrill tcpdrill.c tapdev.c)

target_include_directories(test_tcpdrill PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/src/sana2"
    ${AMINETXDUO_CORE_INCLUDES})

if(AMIGA_NDK_INCLUDE)
    target_include_directories(test_tcpdrill PRIVATE "${AMIGA_NDK_INCLUDE}")
endif()

target_compile_options(test_tcpdrill PRIVATE -Wall -Wextra)

# The window bounds, read out of the header the library compiles against.
# Scripts used to spell 33580 and the numbers derived from it, which made them
# a transcript of one configuration: raising the ceiling failed tcp.drill w01
# and rwndupdate.drill s03 on expectations, not on behaviour.  Read rather than
# repeated, the same way cmake/AmiNetXDuoVersion.cmake reads its pins, so there
# is one place to change and no copy to forget.
set(_anxd_bsd_h "${CMAKE_SOURCE_DIR}/src/bsdsocket/bsdsocket_internal.h")
file(READ "${_anxd_bsd_h}" _anxd_bsd_src)

string(REGEX MATCH "#define[ \t]+BSD_TCP_WINDOW[ \t]+([0-9]+)"
       _anxd_m "${_anxd_bsd_src}")
if(NOT _anxd_m)
    message(FATAL_ERROR "no BSD_TCP_WINDOW default in ${_anxd_bsd_h}")
endif()
set(_anxd_win ${CMAKE_MATCH_1})

# The ceiling is not a literal: it is the budget at the pool's own maximum
# size, so it is read the same way out of the two headers that set it.
string(REGEX MATCH "#define[ \t]+BSD_TCP_WINDOW_POOL_SHARE[ \t]+([0-9]+)"
       _anxd_m "${_anxd_bsd_src}")
if(NOT _anxd_m)
    message(FATAL_ERROR "no BSD_TCP_WINDOW_POOL_SHARE default in ${_anxd_bsd_h}")
endif()
set(_anxd_share ${CMAKE_MATCH_1})

set(_anxd_ns_h "${CMAKE_SOURCE_DIR}/include/aminetxduo/netstack.h")
file(READ "${_anxd_ns_h}" _anxd_ns_src)
string(REGEX MATCH "#define[ \t]+AMI_POOL_PAYLOAD[ \t]+([0-9]+)"
       _anxd_m "${_anxd_ns_src}")
if(NOT _anxd_m)
    message(FATAL_ERROR "no AMI_POOL_PAYLOAD in ${_anxd_ns_h}")
endif()
set(_anxd_payload ${CMAKE_MATCH_1})
string(REGEX MATCH "#define[ \t]+AMI_POOL_MAX_PACKETS[ \t]+([0-9]+)"
       _anxd_m "${_anxd_ns_src}")
if(NOT _anxd_m)
    message(FATAL_ERROR "no AMI_POOL_MAX_PACKETS in ${_anxd_ns_h}")
endif()
set(_anxd_maxpkt ${CMAKE_MATCH_1})

math(EXPR _anxd_wincap "(${_anxd_maxpkt} / ${_anxd_share}) * ${_anxd_payload}")

# AMINETXDUO_TCP_WINDOW pins both, and the library sees it through
# add_compile_definitions, so the drill has to follow it there too.
if(NOT AMINETXDUO_TCP_WINDOW STREQUAL "")
    set(_anxd_win    ${AMINETXDUO_TCP_WINDOW})
    set(_anxd_wincap ${AMINETXDUO_TCP_WINDOW})
endif()

target_compile_definitions(test_tcpdrill PRIVATE
    DRILL_TCP_WINDOW=${_anxd_win}
    DRILL_TCP_WINDOW_CEILING=${_anxd_wincap})

message(STATUS "tcpdrill: window ${_anxd_win}..${_anxd_wincap}")
set_target_properties(test_tcpdrill PROPERTIES OUTPUT_NAME "TcpDrill")

if(AMIGA_SIZE)
    add_custom_command(TARGET test_tcpdrill POST_BUILD
        COMMAND "${AMIGA_SIZE}" "$<TARGET_FILE:test_tcpdrill>"
        COMMENT "TcpDrill size")
endif()
