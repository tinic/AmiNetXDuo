# Feed the netdb alias-pool case to fuzz_config on stdin and require a clean
# exit.  Run through CMake rather than a shell one-liner so it behaves the same
# on every host ctest runs on.
#
# THE CASE: `1.2.3.4 h """"..."""` with thirty-two empty quoted strings.
# netdb_measure() counts the whole run as ONE token; scan_item() returns an
# empty non-NULL token for each "" pair, so the parse pass produces thirty-two.
# alias_pool was sized by the first and written by the second, putting 29
# pointer writes past a 48-byte block for every such line, from a file in
# DEVS:, at library-open time, on a machine with no MMU.
#
# Under ASan this aborts. Without the fix it must abort; with it, it must not.

if(NOT DEFINED DRIVER)
    message(FATAL_ERROR "run-regression.cmake: -DDRIVER=<path to fuzz_config>")
endif()

string(REPEAT "\"" 32 _quotes)
set(_line "1.2.3.4 h ${_quotes}\n")

set(_input "${CMAKE_CURRENT_BINARY_DIR}/fuzz-netdb-regression.in")
file(WRITE "${_input}" "${_line}")

execute_process(
    COMMAND "${DRIVER}" -f 0
    INPUT_FILE "${_input}"
    RESULT_VARIABLE _rc
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE  _err)

if(NOT _rc EQUAL 0)
    message(FATAL_ERROR
        "netdb alias-pool regression FAILED (exit ${_rc}).\n"
        "This is the sizing-pass/parse-pass token mismatch: a run of empty\n"
        "quoted strings counts as one token when the pool is sized and as one\n"
        "per pair when it is written.\n"
        "--- stdout ---\n${_out}\n--- stderr ---\n${_err}")
endif()
