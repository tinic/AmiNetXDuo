# ThreadX AmigaOS/Exec port (docs/RESEARCH.md 6.1, 6.2, 6.3).
#
# These sources replace the eight files every ThreadX port must supply.  They
# are deliberately NOT part of the `threadx` target: the vendored core stays
# untouched, and the port is a separate archive that satisfies the core's
# undefined symbols (_tx_thread_schedule, _tx_thread_system_return,
# _tx_thread_stack_build, _tx_thread_interrupt_disable, ...).

add_library(threadx_port STATIC
    src/tx_initialize_low_level.c
    src/tx_thread_schedule.c
    src/tx_thread_system_return.c
    src/tx_thread_context_save.c
    src/tx_thread_context_restore.c
    src/tx_thread_stack_build.c
    src/tx_thread_interrupt_control.c
    src/tx_timer_interrupt.c
    src/tx_amiga_adopt.c)

target_include_directories(threadx_port
    PUBLIC  "${CMAKE_CURRENT_SOURCE_DIR}/inc"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

if(AMIGA_NDK_INCLUDE)
    target_include_directories(threadx_port PRIVATE "${AMIGA_NDK_INCLUDE}")
endif()

target_link_libraries(threadx_port PUBLIC threadx)

# threadx and threadx_port genuinely depend on each other (the core calls the
# port, the port calls the core).  Declaring the cycle makes CMake repeat both
# archives on the link line rather than leaving dangling symbols.
target_link_libraries(threadx INTERFACE threadx_port)
