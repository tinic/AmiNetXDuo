# tapprobe -- what a FOREIGN stack does with a SANA-II device.
#
# Not a test: nothing here asserts and nothing here ships.  It installs an
# instrumented copy of the tcpdrill device, starts Roadshow (or our own
# library, as the calibration case) against it, and writes down the alignment
# it is handed, how many CMD_READs it keeps outstanding, when it re-arms and
# what its copy hook costs.
#
# Links nothing from src/ -- sana2_device.h is included for the SANA-II
# command numbers, which are protocol facts.

add_executable(tapprobe tapprobe.c probedev.c)

target_include_directories(tapprobe PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/src/sana2"
    ${AMINETXDUO_CORE_INCLUDES})

if(AMIGA_NDK_INCLUDE)
    target_include_directories(tapprobe PRIVATE "${AMIGA_NDK_INCLUDE}")
endif()

target_compile_options(tapprobe PRIVATE -Wall -Wextra)
set_target_properties(tapprobe PROPERTIES OUTPUT_NAME "TapProbe")
