# Host-only regression tests for the toolchain gates themselves.
if(CMAKE_CROSSCOMPILING)
    return()
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(
    NAME toolchain_crt0_gate
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/test_crt0_gate.py")
