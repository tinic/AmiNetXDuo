# Host-only regression tests for the toolchain gates themselves.
if(CMAKE_CROSSCOMPILING)
    return()
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(
    NAME toolchain_crt0_gate
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/test_crt0_gate.py")

# cmake/check-a5-frame.cmake against listings of the frames it must reject
# (the beta5 clobber, an unmatched exg) and the ones it must pass (a stack
# restore, the NDK's exg pair around Supervisor()).
add_test(
    NAME toolchain_a5_frame_gate
    COMMAND "${CMAKE_COMMAND}"
            "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}"
            "-DWORK=${CMAKE_CURRENT_BINARY_DIR}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/check_a5_frame_cases.cmake")
