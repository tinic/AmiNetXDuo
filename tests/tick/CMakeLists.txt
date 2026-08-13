# The tick health counters' unit conversion, without an Amiga.
#
# Host-only: it compiles port/threadx-amiga/inc/tx_amiga.h against ThreadX's
# linux tx_port.h through tests/perf/host/shim, and the exec/types.h it wants
# comes from tests/bsdsocket/host/shim.  Nothing of the port is linked; the
# three functions under test are inlines in that header, which is what lets
# them be read from the tick task and asserted here.

if(CMAKE_CROSSCOMPILING)
    return()
endif()

add_executable(test_tick_conv host/test_tick_conv_host.c)

target_include_directories(test_tick_conv PRIVATE
    "${CMAKE_SOURCE_DIR}/tests/perf/host/shim"          # tx_port.h, first
    "${CMAKE_SOURCE_DIR}/tests/bsdsocket/host/shim"     # exec/types.h
    "${CMAKE_SOURCE_DIR}/port/threadx-amiga/inc"        # tx_amiga.h, ours
    "${AMINETXDUO_THREADX}/common/inc")

target_compile_definitions(test_tick_conv PRIVATE
    AMINETXDUO_HOST_VENDORED_TX_PORT="${AMINETXDUO_THREADX}/ports/linux/gnu/inc/tx_port.h")

include(CTest)
add_test(NAME tick_conv COMMAND test_tick_conv)
