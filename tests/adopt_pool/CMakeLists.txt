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
add_test(NAME adopt_pool_sweep COMMAND test_adopt_pool sweep)
add_test(NAME adopt_pool_evict COMMAND test_adopt_pool evict)

# Which of one Task's threads the Exec-wait bracket suspends and resumes.
# tx_amiga_exec_wait.c is #included whole; netstack_baton.c is compiled as it
# ships, against the NetX Duo HEADERS netstack_internal.h names, no NetX code.
add_executable(test_exec_wait_owner
    host/test_exec_wait_owner_host.c
    "${CMAKE_SOURCE_DIR}/src/netstack/netstack_baton.c")

target_include_directories(test_exec_wait_owner PRIVATE
    "${CMAKE_SOURCE_DIR}/port/threadx-amiga/src"        # tx_amiga_exec_wait.c
    "${CMAKE_SOURCE_DIR}/tests/perf/host/shim"          # tx_port.h, first
    "${CMAKE_SOURCE_DIR}/port/threadx-amiga/inc"
    "${CMAKE_SOURCE_DIR}/tests/bsdsocket/host/shim"     # exec/*, proto/exec.h
    "${CMAKE_SOURCE_DIR}/src/netstack"
    "${CMAKE_SOURCE_DIR}/include"
    "${AMINETXDUO_NETXDUO}/ports/linux/gnu/inc"         # nx_port.h
    "${CMAKE_SOURCE_DIR}/port/netxduo-amiga/inc"        # nx_user.h, ours
    "${AMINETXDUO_NETXDUO}/common/inc"
    "${AMINETXDUO_NETXDUO}/addons/dhcp"
    "${AMINETXDUO_NETXDUO}/addons/dns"
    "${AMINETXDUO_NETXDUO}/addons/auto_ip"
    "${AMINETXDUO_NETXDUO}/addons/mdns"
    "${AMINETXDUO_THREADX}/common/inc")

target_compile_definitions(test_exec_wait_owner PRIVATE
    NX_INCLUDE_USER_DEFINE_FILE
    AMINETXDUO_HOST_VENDORED_TX_PORT="${CMAKE_SOURCE_DIR}/port/threadx-amiga/inc/tx_port.h")

target_compile_options(test_exec_wait_owner PRIVATE -Wno-pointer-sign)

add_test(NAME exec_wait_owner_release COMMAND test_exec_wait_owner release)
add_test(NAME exec_wait_owner_acquire COMMAND test_exec_wait_owner acquire)
add_test(NAME exec_wait_owner_nested COMMAND test_exec_wait_owner nested)
