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
    src/tx_amiga_adopt.c
    src/tx_amiga_green.c)

# The green realm's m68k context switch.  The .S is #ifdef'd end to end on
# AMINETXDUO_GREEN_REALM (a global compile definition when the option is ON),
# and m68k-only, so it joins the target only on a cross build with the realm
# selected.
if(AMINETXDUO_GREEN_REALM AND CMAKE_CROSSCOMPILING)
    target_sources(threadx_port PRIVATE src/tx_green_switch.S)
    # The define must reach the .S through the preprocessor the same way the
    # net68k assembly gets its own (src/net68k/CMakeLists.txt): a source
    # property, so the file assembles identically whatever the global
    # definition set happens to be.
    set_source_files_properties(src/tx_green_switch.S PROPERTIES
        COMPILE_DEFINITIONS AMINETXDUO_GREEN_REALM)
endif()

target_include_directories(threadx_port
    PUBLIC  "${CMAKE_CURRENT_SOURCE_DIR}/inc"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
            "${CMAKE_SOURCE_DIR}/include")

if(AMIGA_NDK_INCLUDE)
    target_include_directories(threadx_port PRIVATE "${AMIGA_NDK_INCLUDE}")
endif()

target_link_libraries(threadx_port PUBLIC threadx)

# The tick task reports its wakeup source, its fallback and its stalls through
# ami_log().  That is a real dependency on aminetxduo_common, and declaring it
# is what makes CMake repeat the archive after threadx_port on the link line --
# several targets list aminetxduo_common first, where ld would never find it.
target_link_libraries(threadx_port PRIVATE aminetxduo_common)

# threadx and threadx_port genuinely depend on each other (the core calls the
# port, the port calls the core).  Declaring the cycle makes CMake repeat both
# archives on the link line rather than leaving dangling symbols.
target_link_libraries(threadx INTERFACE threadx_port)
