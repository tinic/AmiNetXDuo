# Milestone 2 bring-up test: ThreadX on Exec + NetX Duo over the in-tree
# simulated RAM driver.  Produces an AmigaOS executable to be run under
# FS-UAE/WinUAE (or on real hardware); it is not runnable on the build host.

add_executable(ram_driver_test ram_driver_test.c)

target_link_libraries(ram_driver_test PRIVATE netxduo_port)

if(AMIGA_NDK_INCLUDE)
    target_include_directories(ram_driver_test PRIVATE "${AMIGA_NDK_INCLUDE}")
endif()

set_target_properties(ram_driver_test PROPERTIES OUTPUT_NAME "ram_driver_test")

if(AMIGA_SIZE)
    add_custom_command(TARGET ram_driver_test POST_BUILD
        COMMAND "${AMIGA_SIZE}" "$<TARGET_FILE:ram_driver_test>"
        COMMENT "ram_driver_test size")
endif()
