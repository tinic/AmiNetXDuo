# The ThreadX/Exec adoption layer on its own -- docs/RESEARCH.md 79's defect,
# reduced to the layer it lived in.
#
# Links threadx_port and nothing else of the stack: no NetX Duo, no
# bsdsocket.library, no SANA-II driver, no interface. That is what lets this run
# in public CI, where every harness that reaches the library through its LVOs
# needs a2065.device and stays out.
#
# Not to be confused with tests/perf/bracket_test.c, which measures what a
# bracket COSTS (RESEARCH 29.3, 32.11). That one prices the layer; this one says
# whether it is correct.

if(NOT CMAKE_CROSSCOMPILING)
    return()
endif()

# netstack_baton.c is compiled in rather than linked from aminetxduo_netstack:
# the bracket is the thing under test and the rest of that library is the stack
# this file exists to stay out of. It needs NetX Duo HEADERS, because
# netstack_internal.h declares the singleton in terms of them -- no NetX Duo
# code is linked.
add_executable(bracket_invariants
    bracket_test.c
    "${CMAKE_SOURCE_DIR}/src/netstack/netstack_baton.c")

target_link_libraries(bracket_invariants PRIVATE threadx_port aminetxduo_common)

target_include_directories(bracket_invariants PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/src/netstack"
    ${AMINETXDUO_CORE_INCLUDES}
    ${NETXDUO_ADDON_INCLUDES})

if(AMIGA_NDK_INCLUDE)
    target_include_directories(bracket_invariants PRIVATE "${AMIGA_NDK_INCLUDE}")
endif()

# The churn's size. Small enough for CI, and an axis to turn up when hunting:
# the defect this was written for needed the baton to be held by somebody else
# at the moment a second Task asked, so more contention is more chances.
set(BRACKET_WORKERS "6"   CACHE STRING "Unrelated Exec Tasks in the churn phase")
set(BRACKET_ROUNDS  "200" CACHE STRING "Adopt/orphan cycles per worker")

# The baton phase runs on wall-clock time, so its axis is seconds, not rounds:
# the probe samples 500 times a second and a longer phase is more samples.
set(BRACKET_BATON_TASKS "3"   CACHE STRING "Tasks in the release/acquire phase")
set(BRACKET_BATON_TICKS "250" CACHE STRING "Ticks the release/acquire phase runs")

target_compile_definitions(bracket_invariants PRIVATE
    BT_WORKERS=${BRACKET_WORKERS}
    BT_ROUNDS=${BRACKET_ROUNDS}
    BT_BATON_TASKS=${BRACKET_BATON_TASKS}
    BT_BATON_TICKS=${BRACKET_BATON_TICKS})

set_target_properties(bracket_invariants PROPERTIES OUTPUT_NAME "bracket_invariants")
