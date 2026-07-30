# Eight applications inside the stack at once -- the gap docs/RESEARCH.md 78's
# baton defect went through.
#
# Produces an AmigaOS executable; it needs the library it is testing staged
# next to it, like the other on-Amiga harnesses.
#
# Links nothing of the stack: it reaches bsdsocket.library through its LVOs,
# because a program that linked src/netstack would get a SECOND set of NetX Duo
# globals and measure the wrong stack (include/aminetxduo/netstatus.h).

if(NOT CMAKE_CROSSCOMPILING)
    return()
endif()

add_executable(concurrent_test concurrent_test.c)

target_include_directories(concurrent_test PRIVATE
    "${CMAKE_SOURCE_DIR}/include")

if(AMIGA_NDK_INCLUDE)
    target_include_directories(concurrent_test PRIVATE "${AMIGA_NDK_INCLUDE}")
endif()

set_target_properties(concurrent_test PROPERTIES
    OUTPUT_NAME "concurrent_test")

# The shape is a build option because the interesting axis is how many bases
# are inside the bracket at once, and the answer that fits CI is not
# necessarily the answer that reproduces a defect. Four pairs is what runs in
# EMULATOR_TESTS; raise it when hunting.
set(CONCURRENT_PAIRS "4" CACHE STRING
    "Server/client pairs; applications = 2x this")
set(CONCURRENT_BYTES "32768" CACHE STRING
    "Bytes each way per pair")

target_compile_definitions(concurrent_test PRIVATE
    CT_PAIRS=${CONCURRENT_PAIRS}
    CT_BYTES=${CONCURRENT_BYTES}UL)
