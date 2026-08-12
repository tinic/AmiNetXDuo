# Milestone 1 exit criterion: adoption soak (docs/RESEARCH.md 8).
#
# Produces an AmigaOS executable to be run under FS-UAE/WinUAE (or on real
# hardware); it is not runnable on the build host.  Run it with
#
#   AMINETXDUO_RUN_TAG=soak ./tools/amiberry-run.sh -t 220 build/cm/tests/soak/soak_test
#
# and, for the "clean under Enforcer" half of the criterion, with
#
#   ./tools/enforcer-run.sh -m -t 260 build/cm/tests/soak/soak_test
#
# The run takes about 100 s: a 30 s soak plus the wait-abort, suspend/resume and
# teardown phases, which wait on real timeouts.

add_executable(soak_test soak_test.c)

# Bisecting knobs, e.g. -DSOAK_DEFS="S_SOAK_SECONDS=15;S_SKIP_ABORT=1".
set(SOAK_DEFS "" CACHE STRING "Extra -D defines for the soak test")
if(NOT SOAK_DEFS STREQUAL "")
    target_compile_definitions(soak_test PRIVATE ${SOAK_DEFS})
endif()

target_link_libraries(soak_test PRIVATE threadx_port aminetxduo_common)

if(AMIGA_NDK_INCLUDE)
    target_include_directories(soak_test PRIVATE "${AMIGA_NDK_INCLUDE}")
endif()

set_target_properties(soak_test PROPERTIES OUTPUT_NAME "soak_test")

if(AMIGA_SIZE)
    add_custom_command(TARGET soak_test POST_BUILD
        COMMAND "${AMIGA_SIZE}" "$<TARGET_FILE:soak_test>"
        COMMENT "soak_test size")
endif()
