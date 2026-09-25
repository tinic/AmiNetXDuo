# The adoption slot pool's identity checks, without an Amiga.
#
# Host-only.  port/threadx-amiga/src/tx_amiga_pool.c is #included whole and
# compiled against the real tx_amiga_internal.h: the port's tx_port.h through
# tests/perf/host/shim, which gives it the m68k's type widths, and Exec from
# tests/bsdsocket/host/shim.  The test file fakes the Exec calls it reaches.

if(CMAKE_CROSSCOMPILING)
    return()
endif()

add_executable(test_adopt_pool host/test_adopt_pool_host.c)

target_include_directories(test_adopt_pool PRIVATE
    "${CMAKE_SOURCE_DIR}/port/threadx-amiga/src"        # tx_amiga_pool.c
    "${CMAKE_SOURCE_DIR}/tests/perf/host/shim"          # tx_port.h, first
    "${CMAKE_SOURCE_DIR}/port/threadx-amiga/inc"
    "${CMAKE_SOURCE_DIR}/tests/bsdsocket/host/shim"     # exec/*, proto/exec.h
    "${CMAKE_SOURCE_DIR}/include"
    "${AMINETXDUO_THREADX}/common/inc")

target_compile_definitions(test_adopt_pool PRIVATE
    AMINETXDUO_HOST_VENDORED_TX_PORT="${CMAKE_SOURCE_DIR}/port/threadx-amiga/inc/tx_port.h")

# FindTask((STRPTR) 0) in the pool against the shim's const char * prototype.
target_compile_options(test_adopt_pool PRIVATE -Wno-pointer-sign)

include(CTest)
add_test(NAME adopt_pool_wake COMMAND test_adopt_pool wake)
