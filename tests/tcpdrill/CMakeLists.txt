# tcpdrill -- packet-level TCP conformance.
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
# command numbers and structures alone -- they are protocol facts, and the
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
set_target_properties(test_tcpdrill PROPERTIES OUTPUT_NAME "TcpDrill")

if(AMIGA_SIZE)
    add_custom_command(TARGET test_tcpdrill POST_BUILD
        COMMAND "${AMIGA_SIZE}" "$<TARGET_FILE:test_tcpdrill>"
        COMMENT "TcpDrill size")
endif()
