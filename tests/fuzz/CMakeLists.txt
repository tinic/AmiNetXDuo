# Sanitizer fuzz drivers for the parsers that read bytes we did not write.
#
# WHY THIS EXISTS AT ALL, AND WHY IT IS HOST-ONLY
#
# The target has no MMU and cannot run Enforcer, so a stray write does not
# fault -- it corrupts another task and surfaces later as something unrelated.
# There is no crash to bisect and no sanitizer to run there.
#
# The parsers, though, are portable C. They can be compiled for the host under
# ASan and UBSan and driven with malformed input, which is exactly the tooling
# the Amiga cannot provide. That is how the netdb alias-pool overflow was
# found: the sizing pass counted a run of non-whitespace as one token while the
# parse pass saw one token per "" pair, so `1.2.3.4 h """"..."""` wrote 29
# pointers past a 48-byte block -- silently, on every library open, from a file
# in DEVS:.
#
# THE REGRESSION CASE RUNS EVERY TIME. The random sweep is bounded so `ctest`
# stays quick; AMINETXDUO_FUZZ_CASES raises it for a real soak:
#
#     AMINETXDUO_FUZZ_CASES=200000 ctest -R fuzz
#
# Sanitizers are probed rather than assumed -- a host toolchain without them
# still configures, and says so, rather than failing the build.

if(CMAKE_CROSSCOMPILING)
    return()
endif()

include(CheckCSourceCompiles)

set(_fuzz_san -fsanitize=address,undefined -fno-sanitize-recover=all
              -fno-omit-frame-pointer)

set(CMAKE_REQUIRED_FLAGS "-fsanitize=address,undefined")
check_c_source_compiles("int main(void){return 0;}" AMINETXDUO_HAVE_SANITIZERS)
unset(CMAKE_REQUIRED_FLAGS)

if(NOT AMINETXDUO_HAVE_SANITIZERS)
    message(STATUS "fuzz: no ASan/UBSan in this host compiler -- drivers skipped")
    return()
endif()

# How many random cases per driver under a plain `ctest`. Small on purpose:
# this is a regression gate, not the soak. The soak is the env var above.
set(AMINETXDUO_FUZZ_CASES "2000" CACHE STRING
    "Random cases per fuzz driver under ctest")

# The config parsers: DEVS:Internet/{hosts,networks,protocols,services} and
# DEVS:NetInterfaces/*.
add_executable(fuzz_config
    fuzz_config.c
    "${CMAKE_SOURCE_DIR}/src/config/config_text.c"
    "${CMAKE_SOURCE_DIR}/src/config/config_text6.c"
    "${CMAKE_SOURCE_DIR}/src/config/config_parse.c"
    "${CMAKE_SOURCE_DIR}/src/config/netdb.c")

target_include_directories(fuzz_config PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/src/config"
    "${CMAKE_SOURCE_DIR}/src/config/test/shim")

target_compile_options(fuzz_config PRIVATE -Wall -Wextra ${_fuzz_san})
target_link_options(fuzz_config PRIVATE ${_fuzz_san})

# The BPF filter VM, which runs programs a caller supplies over packets the
# wire supplies. The driver deliberately runs UNVALIDATED programs, and
# wirelen > caplen, and short buffers -- the interpreter claims to be total and
# this is what tests that claim rather than repeating it.
add_executable(fuzz_bpf
    fuzz_bpf.c
    "${CMAKE_SOURCE_DIR}/src/bpf/bpf_filter.c"
    "${CMAKE_SOURCE_DIR}/src/bpf/bpf_validate.c"
    "${CMAKE_SOURCE_DIR}/src/bpf/bpf_channel.c"
    "${CMAKE_SOURCE_DIR}/src/bpf/bpf_tap.c")

target_include_directories(fuzz_bpf PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/src/bpf"
    "${CMAKE_SOURCE_DIR}/src/config/test/shim")

target_compile_definitions(fuzz_bpf PRIVATE AMI_BPF_REPLICA)
target_compile_options(fuzz_bpf PRIVATE -Wall -Wextra ${_fuzz_san})
target_link_options(fuzz_bpf PRIVATE ${_fuzz_san})

include(CTest)

# The known case, first and always. If this ever passes silently the whole
# point of the file has been lost.
add_test(NAME fuzz_netdb_regression
         COMMAND ${CMAKE_COMMAND}
                 -DDRIVER=$<TARGET_FILE:fuzz_config>
                 -P "${CMAKE_CURRENT_SOURCE_DIR}/run-regression.cmake")

add_test(NAME fuzz_config_sweep
         COMMAND fuzz_config -r 1 ${AMINETXDUO_FUZZ_CASES})
add_test(NAME fuzz_bpf_sweep
         COMMAND fuzz_bpf -r 1 ${AMINETXDUO_FUZZ_CASES})

# A soak set by AMINETXDUO_FUZZ_CASES needs longer than ctest's default.
set_tests_properties(fuzz_config_sweep fuzz_bpf_sweep PROPERTIES TIMEOUT 1800)
