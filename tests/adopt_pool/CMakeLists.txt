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
add_test(NAME soak_orphan_baton_identity COMMAND test_adopt_pool orphan-baton)

# tx_amiga_adopt_suspend()'s run-signal clear: a SetSignal() only when the bit
# is latched.  tx_amiga_adopt.c is #included whole, the same way.
add_executable(test_adopt_suspend host/test_adopt_suspend_host.c)

target_include_directories(test_adopt_suspend PRIVATE
    "${CMAKE_SOURCE_DIR}/port/threadx-amiga/src"        # tx_amiga_adopt.c
    "${CMAKE_SOURCE_DIR}/tests/perf/host/shim"          # tx_port.h, first
    "${CMAKE_SOURCE_DIR}/port/threadx-amiga/inc"
    "${CMAKE_SOURCE_DIR}/tests/bsdsocket/host/shim"     # exec/*, proto/exec.h
    "${CMAKE_SOURCE_DIR}/include"
    "${AMINETXDUO_THREADX}/common/inc")

target_compile_definitions(test_adopt_suspend PRIVATE
    AMINETXDUO_HOST_VENDORED_TX_PORT="${CMAKE_SOURCE_DIR}/port/threadx-amiga/inc/tx_port.h")

target_compile_options(test_adopt_suspend PRIVATE -Wno-pointer-sign)

add_test(NAME adopt_suspend_clean COMMAND test_adopt_suspend clean)
add_test(NAME adopt_suspend_latched COMMAND test_adopt_suspend latched)

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

# _tx_thread_create() against the port's TX_THREAD_STACK_BUILD_STATUS: a builder
# that leaves no Exec Task must fail the create.  Only our ThreadX fork has the
# hook; plain upstream returns TX_SUCCESS.  tx_thread_create.c is #included whole.
add_executable(test_thread_create_status host/test_thread_create_status_host.c)

target_include_directories(test_thread_create_status PRIVATE
    "${AMINETXDUO_THREADX}/common/src"                  # tx_thread_create.c
    "${CMAKE_SOURCE_DIR}/tests/perf/host/shim"          # tx_port.h, first
    "${CMAKE_SOURCE_DIR}/port/threadx-amiga/inc"
    "${CMAKE_SOURCE_DIR}/tests/bsdsocket/host/shim"
    "${CMAKE_SOURCE_DIR}/include"
    "${AMINETXDUO_THREADX}/common/inc")

target_compile_definitions(test_thread_create_status PRIVATE
    AMINETXDUO_HOST_VENDORED_TX_PORT="${CMAKE_SOURCE_DIR}/port/threadx-amiga/inc/tx_port.h")

# The m68k ULONG is 32 bits; the core's pointer-to-ULONG casts warn on a
# 64-bit host and are never dereferenced here.
target_compile_options(test_thread_create_status PRIVATE
    -Wno-pointer-sign -Wno-pointer-to-int-cast)

add_test(NAME thread_create_status_fail COMMAND test_thread_create_status fail)
add_test(NAME thread_create_status_ok COMMAND test_thread_create_status ok)

# Actual allocator/ownership paths, with both the standalone weak default and
# the shared library's strong zero-arena definition on the link line.
foreach(_arena IN ITEMS standalone library)
    add_executable(test_application_memory_${_arena}
        host/test_application_memory_host.c
        "${CMAKE_SOURCE_DIR}/port/threadx-amiga/src/tx_amiga_memory.c")
    target_include_directories(test_application_memory_${_arena} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/host/memory-shim"
        "${CMAKE_SOURCE_DIR}/port/threadx-amiga/src"
        "${CMAKE_SOURCE_DIR}/tests/perf/host/shim"
        "${CMAKE_SOURCE_DIR}/port/threadx-amiga/inc"
        "${CMAKE_SOURCE_DIR}/tests/bsdsocket/host/shim"
        "${CMAKE_SOURCE_DIR}/include"
        "${AMINETXDUO_THREADX}/common/inc")
    target_compile_definitions(test_application_memory_${_arena} PRIVATE
        AMINETXDUO_HOST_VENDORED_TX_PORT="${CMAKE_SOURCE_DIR}/port/threadx-amiga/inc/tx_port.h")
    if(_arena STREQUAL "library")
        target_sources(test_application_memory_${_arena} PRIVATE
            "${CMAKE_SOURCE_DIR}/src/bsdsocket/application_memory.c")
        target_compile_definitions(test_application_memory_${_arena} PRIVATE
            EXPECTED_ARENA_SIZE=0UL)
    else()
        target_compile_definitions(test_application_memory_${_arena} PRIVATE
            EXPECTED_ARENA_SIZE=32768UL)
    endif()
    add_test(NAME application_memory_${_arena}
        COMMAND test_application_memory_${_arena})
endforeach()
