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
add_library(research_tx_bridge STATIC tx_bridge.c netx_resume.c netx_packet.c
    "${ANX_ROOT}/third_party/threadx/common/src/tx_thread_timeout.c"
    "${ANX_ROOT}/third_party/threadx/common/src/tx_thread_sleep.c"
    "${ANX_ROOT}/third_party/threadx/common/src/tx_thread_wait_abort.c"
    "${ANX_ROOT}/third_party/threadx/common/src/tx_mutex_cleanup.c"
    "${ANX_ROOT}/third_party/threadx/common/src/tx_event_flags_get.c"
    "${ANX_ROOT}/third_party/threadx/common/src/tx_event_flags_set.c"
    "${ANX_ROOT}/third_party/threadx/common/src/tx_event_flags_cleanup.c"
    "${ANX_ROOT}/third_party/threadx/common/src/tx_timer_create.c"
    "${ANX_ROOT}/third_party/threadx/common/src/tx_timer_activate.c"
    "${ANX_ROOT}/third_party/threadx/common/src/tx_timer_change.c"
    "${ANX_ROOT}/third_party/threadx/common/src/tx_timer_delete.c"
    "${ANX_ROOT}/third_party/netxduo/common/src/nx_ip_periodic_timer_entry.c"
    "${ANX_ROOT}/third_party/netxduo/common/src/nx_tcp_socket_thread_suspend.c"
    "${ANX_ROOT}/third_party/netxduo/common/src/nx_tcp_socket_thread_resume.c"
    "${ANX_ROOT}/third_party/netxduo/common/src/nx_tcp_receive_cleanup.c"
    "${ANX_ROOT}/third_party/netxduo/common/src/nx_tcp_cleanup_deferred.c"
    "${ANX_ROOT}/third_party/netxduo/common/src/nx_tcp_deferred_cleanup_check.c"
    "${ANX_ROOT}/third_party/netxduo/common/src/nx_tcp_client_bind_cleanup.c"
    "${ANX_ROOT}/third_party/netxduo/common/src/nx_tcp_connect_cleanup.c"
    "${ANX_ROOT}/third_party/netxduo/common/src/nx_tcp_disconnect_cleanup.c"
    "${ANX_ROOT}/third_party/netxduo/common/src/nx_tcp_transmit_cleanup.c"
    "${ANX_ROOT}/third_party/netxduo/common/src/nx_udp_socket_receive.c"
    "${ANX_ROOT}/third_party/netxduo/common/src/nx_udp_receive_cleanup.c"
    "${ANX_ROOT}/third_party/netxduo/common/src/nx_ip_checksum_compute.c"
    "${ANX_ROOT}/third_party/netxduo/common/src/nx_packet_release.c"
    "${ANX_ROOT}/third_party/netxduo/common/src/nx_packet_pool_initialize.c"
    "${ANX_ROOT}/third_party/netxduo/common/src/nx_packet_pool_create.c"
    "${ANX_ROOT}/third_party/netxduo/common/src/nx_packet_pool_delete.c"
    "${ANX_ROOT}/third_party/netxduo/common/src/nx_packet_allocate.c"
    "${ANX_ROOT}/third_party/netxduo/common/src/nx_packet_pool_cleanup.c"
    "${ANX_ROOT}/third_party/netxduo/common/src/nx_packet_data_append.c"
    "${ANX_ROOT}/third_party/netxduo/common/src/nx_packet_copy.c"
    "${ANX_ROOT}/third_party/netxduo/common/src/nx_udp_packet_receive.c")
target_link_libraries(research_tx_bridge PUBLIC research_wait)
target_include_directories(research_tx_bridge PUBLIC ${ANX_BRIDGE_INCLUDES})
target_compile_definitions(research_tx_bridge PUBLIC TX_DISABLE_ERROR_CHECKING)
target_compile_options(research_tx_bridge PRIVATE -Wall -Wextra -Werror)
set_source_files_properties("${ANX_ROOT}/third_party/threadx/common/src/tx_thread_wait_abort.c"
    PROPERTIES COMPILE_DEFINITIONS "_tx_thread_wait_abort=anx_tx_original_wait_abort")
foreach(service event_flags_get event_flags_set timer_create timer_activate timer_change timer_delete)
    set_source_files_properties("${ANX_ROOT}/third_party/threadx/common/src/tx_${service}.c"
        PROPERTIES COMPILE_DEFINITIONS "_tx_${service}=anx_tx_original_${service}")
endforeach()
foreach(service packet_allocate packet_pool_delete)
    set_source_files_properties("${ANX_ROOT}/third_party/netxduo/common/src/nx_${service}.c"
        PROPERTIES COMPILE_DEFINITIONS "_nx_${service}=anx_nx_original_${service}")
endforeach()
if(CMAKE_CROSSCOMPILING)
    enable_language(ASM)
    add_library(research_exec_adapter STATIC exec_wait.c)
    target_link_libraries(research_exec_adapter PUBLIC research_wait)
    target_include_directories(research_exec_adapter PRIVATE "${AMIGA_NDK_INCLUDE}")
    target_compile_options(research_exec_adapter PRIVATE -Wall -Wextra -Werror)
    add_executable(research_exec_task_smoke "${ANX_ROOT}/src/tools/tool_startup.S"
        tx_bridge_exec.c exec_task.c exec_task_smoke.c)
    target_link_libraries(research_exec_task_smoke PRIVATE research_tx_bridge research_exec_adapter)
    target_include_directories(research_exec_task_smoke PRIVATE "${AMIGA_NDK_INCLUDE}")
    target_compile_options(research_exec_task_smoke PRIVATE -Wall -Wextra -Werror)
    target_link_options(research_exec_task_smoke PRIVATE -nostartfiles
        "-Wl,-Map=$<TARGET_FILE:research_exec_task_smoke>.map")
    add_custom_command(TARGET research_exec_task_smoke POST_BUILD
        COMMAND "${CMAKE_COMMAND}"
            "-DTOOL_MAP=$<TARGET_FILE:research_exec_task_smoke>.map"
            -P "${ANX_ROOT}/cmake/check-startup-first.cmake")
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
    find_package(Threads REQUIRED)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_executable(research_wait_model test_wait.c)
    target_link_libraries(research_wait_model PRIVATE research_wait)
    target_compile_options(research_wait_model PRIVATE -Wall -Wextra -Werror)
    add_test(NAME research_wait_model COMMAND research_wait_model)
    add_executable(research_tx_bridge_model test_tx_bridge.c)
    target_link_libraries(research_tx_bridge_model PRIVATE research_tx_bridge)
    target_compile_options(research_tx_bridge_model PRIVATE -Wall -Wextra -Werror)
    add_test(NAME research_tx_bridge_model COMMAND research_tx_bridge_model)
    add_executable(research_mutex_bridge_model test_mutex_bridge.c)
    target_link_libraries(research_mutex_bridge_model PRIVATE research_tx_bridge Threads::Threads)
    target_compile_options(research_mutex_bridge_model PRIVATE -Wall -Wextra -Werror)
    add_test(NAME research_mutex_bridge_model COMMAND research_mutex_bridge_model)
    set_tests_properties(research_mutex_bridge_model PROPERTIES TIMEOUT 15)
    add_executable(research_event_timer_model test_event_timer.c)
    target_link_libraries(research_event_timer_model PRIVATE research_tx_bridge Threads::Threads)
    target_compile_options(research_event_timer_model PRIVATE -Wall -Wextra -Werror)
    add_test(NAME research_event_timer_model COMMAND research_event_timer_model)
    set_tests_properties(research_event_timer_model PROPERTIES TIMEOUT 15)
    foreach(guard nested-get timer-get tick-context callback-delete)
        add_test(NAME research_event_timer_reject_${guard}
            COMMAND research_event_timer_model --reject-${guard})
    endforeach()
    add_executable(research_packet_pool_model test_packet_pool.c)
    target_link_libraries(research_packet_pool_model PRIVATE research_tx_bridge Threads::Threads)
    target_compile_options(research_packet_pool_model PRIVATE -Wall -Wextra -Werror)
    add_test(NAME research_packet_pool_model COMMAND research_packet_pool_model)
    set_tests_properties(research_packet_pool_model PROPERTIES TIMEOUT 15)
    foreach(guard timer-allocate nested-allocate preempt-allocate outstanding-delete)
        add_test(NAME research_packet_pool_reject_${guard}
            COMMAND research_packet_pool_model --reject-${guard})
    endforeach()
    add_executable(research_netx_resume_model test_netx_resume.c)
    target_link_libraries(research_netx_resume_model PRIVATE research_tx_bridge)
    target_compile_options(research_netx_resume_model PRIVATE -Wall -Wextra -Werror)
    add_test(NAME research_netx_resume_model COMMAND research_netx_resume_model)
    add_test(NAME research_netx_resume_reject_sequence
        COMMAND research_netx_resume_model --reject-sequence)
    add_test(NAME research_netx_resume_reject_callback
        COMMAND research_netx_resume_model --reject-callback)
    add_test(NAME research_netx_resume_reject_invalid_socket
        COMMAND research_netx_resume_model --reject-invalid-socket)
    add_test(NAME research_netx_resume_reject_contention
        COMMAND research_netx_resume_model --reject-contention)
    add_executable(research_udp_bridge_model test_udp_bridge.c)
    target_link_libraries(research_udp_bridge_model PRIVATE research_tx_bridge)
    target_compile_options(research_udp_bridge_model PRIVATE -Wall -Wextra -Werror)
    add_test(NAME research_udp_bridge_model COMMAND research_udp_bridge_model)
    add_executable(research_thread_sleep_model test_thread_sleep.c)
    target_link_libraries(research_thread_sleep_model PRIVATE research_tx_bridge)
    target_compile_options(research_thread_sleep_model PRIVATE -Wall -Wextra -Werror)
    add_test(NAME research_thread_sleep_model COMMAND research_thread_sleep_model)
    add_test(NAME research_tx_bridge_reject_stuck_cleanup
        COMMAND research_tx_bridge_model --reject-stuck-cleanup)
    add_test(NAME research_tx_bridge_reject_blocking_mutex
        COMMAND research_tx_bridge_model --reject-blocking-mutex)
    add_test(NAME research_tx_bridge_reject_timer_mutex
        COMMAND research_tx_bridge_model --reject-timer-mutex)
    add_test(NAME research_tx_bridge_reject_foreign_release
        COMMAND research_tx_bridge_model --reject-foreign-release)
    add_test(NAME research_contract_extraction
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_contract.py"
            "${CMAKE_CURRENT_BINARY_DIR}" "${CMAKE_C_COMPILER}")
endif()
