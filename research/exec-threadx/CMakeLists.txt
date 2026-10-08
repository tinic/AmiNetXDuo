cmake_minimum_required(VERSION 3.20)
project(ExecThreadXResearch C)
get_filename_component(ANX_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(CMAKE_C_FLAGS_RELEASE "-Os -DNDEBUG" CACHE STRING "Research flags" FORCE)
add_library(research_wait STATIC wait.c)
target_include_directories(research_wait PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(research_wait PRIVATE -Wall -Wextra -Werror)
set(ANX_BRIDGE_INCLUDES "${ANX_ROOT}/port/threadx-amiga/inc"
    "${ANX_ROOT}/port/netxduo-amiga/inc" "${ANX_ROOT}/third_party/threadx/common/inc"
    "${ANX_ROOT}/third_party/netxduo/common/inc" "${ANX_ROOT}/include")
if(NOT CMAKE_CROSSCOMPILING)
    list(PREPEND ANX_BRIDGE_INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}/host")
endif()
add_library(research_tx_bridge STATIC tx_bridge.c
    "${ANX_ROOT}/third_party/threadx/common/src/tx_thread_timeout.c"
    "${ANX_ROOT}/third_party/threadx/common/src/tx_thread_wait_abort.c"
    "${ANX_ROOT}/third_party/netxduo/common/src/nx_tcp_socket_thread_suspend.c"
    "${ANX_ROOT}/third_party/netxduo/common/src/nx_tcp_socket_thread_resume.c"
    "${ANX_ROOT}/third_party/netxduo/common/src/nx_tcp_receive_cleanup.c"
    "${ANX_ROOT}/third_party/netxduo/common/src/nx_tcp_cleanup_deferred.c"
    "${ANX_ROOT}/third_party/netxduo/common/src/nx_tcp_deferred_cleanup_check.c"
    "${ANX_ROOT}/third_party/netxduo/common/src/nx_tcp_client_bind_cleanup.c"
    "${ANX_ROOT}/third_party/netxduo/common/src/nx_tcp_connect_cleanup.c"
    "${ANX_ROOT}/third_party/netxduo/common/src/nx_tcp_disconnect_cleanup.c"
    "${ANX_ROOT}/third_party/netxduo/common/src/nx_tcp_transmit_cleanup.c")
target_link_libraries(research_tx_bridge PUBLIC research_wait)
target_include_directories(research_tx_bridge PUBLIC ${ANX_BRIDGE_INCLUDES})
target_compile_definitions(research_tx_bridge PUBLIC TX_DISABLE_ERROR_CHECKING)
target_compile_options(research_tx_bridge PRIVATE -Wall -Wextra -Werror)
if(CMAKE_CROSSCOMPILING)
    enable_language(ASM)
    add_library(research_exec_adapter STATIC exec_wait.c)
    target_link_libraries(research_exec_adapter PUBLIC research_wait)
    target_include_directories(research_exec_adapter PRIVATE "${AMIGA_NDK_INCLUDE}")
    target_compile_options(research_exec_adapter PRIVATE -Wall -Wextra -Werror)
    add_executable(research_tx_bridge_smoke "${ANX_ROOT}/src/tools/tool_startup.S"
        tx_bridge_exec.c tx_bridge_smoke.c)
    target_link_libraries(research_tx_bridge_smoke PRIVATE research_tx_bridge research_exec_adapter amiga)
    target_include_directories(research_tx_bridge_smoke PRIVATE "${AMIGA_NDK_INCLUDE}")
    target_compile_options(research_tx_bridge_smoke PRIVATE -Wall -Wextra -Werror)
    target_link_options(research_tx_bridge_smoke PRIVATE -nostartfiles
        "-Wl,-Map=$<TARGET_FILE:research_tx_bridge_smoke>.map")
    add_custom_command(TARGET research_tx_bridge_smoke POST_BUILD
        COMMAND "${CMAKE_COMMAND}"
            "-DTOOL_MAP=$<TARGET_FILE:research_tx_bridge_smoke>.map"
            -P "${ANX_ROOT}/cmake/check-startup-first.cmake")
    # Use the project's command startup and DOS IO: the toolchain crt0/newlib
    # smoke crashed before its first output on the 8192-byte emulator stack.
    add_executable(research_exec_wait_smoke
        "${ANX_ROOT}/src/tools/tool_startup.S" exec_wait_smoke.c)
    target_link_libraries(research_exec_wait_smoke PRIVATE research_exec_adapter)
    target_include_directories(research_exec_wait_smoke PRIVATE "${AMIGA_NDK_INCLUDE}")
    target_compile_options(research_exec_wait_smoke PRIVATE -Wall -Wextra -Werror)
    target_link_options(research_exec_wait_smoke PRIVATE -nostartfiles
        "-Wl,-Map=$<TARGET_FILE:research_exec_wait_smoke>.map")
    add_custom_command(TARGET research_exec_wait_smoke POST_BUILD
        COMMAND "${CMAKE_COMMAND}"
            "-DTOOL_MAP=$<TARGET_FILE:research_exec_wait_smoke>.map"
            -P "${ANX_ROOT}/cmake/check-startup-first.cmake")
    add_library(research_contract_probe OBJECT contract_probe.c)
    target_include_directories(research_contract_probe PRIVATE
        "${ANX_ROOT}/port/threadx-amiga/inc" "${ANX_ROOT}/port/netxduo-amiga/inc"
        "${ANX_ROOT}/third_party/threadx/common/inc"
        "${ANX_ROOT}/third_party/netxduo/common/inc" "${ANX_ROOT}/include"
        "${AMIGA_NDK_INCLUDE}")
else()
    enable_testing()
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_executable(research_wait_model test_wait.c)
    target_link_libraries(research_wait_model PRIVATE research_wait)
    target_compile_options(research_wait_model PRIVATE -Wall -Wextra -Werror)
    add_test(NAME research_wait_model COMMAND research_wait_model)
    add_executable(research_tx_bridge_model test_tx_bridge.c)
    target_link_libraries(research_tx_bridge_model PRIVATE research_tx_bridge)
    target_compile_options(research_tx_bridge_model PRIVATE -Wall -Wextra -Werror)
    add_test(NAME research_tx_bridge_model COMMAND research_tx_bridge_model)
    add_test(NAME research_tx_bridge_reject_deferred_abort
        COMMAND research_tx_bridge_model --reject-deferred-abort)
    add_test(NAME research_contract_extraction
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_contract.py"
            "${CMAKE_CURRENT_BINARY_DIR}" "${CMAKE_C_COMPILER}")
endif()
