# Compile the public SDK with the cross compiler; run the toolchain gate's
# own regression tests in host builds.
if(CMAKE_CROSSCOMPILING)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_custom_target(check_ipv6_sdk ALL
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/check_ipv6_sdk.py"
                --toolchain "${AMIGA_TOOLCHAIN_ROOT}"
        COMMENT "Checking the staged IPv6 SDK ABI and include order")
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
